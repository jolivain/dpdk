/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2020 Intel Corporation
 */

#include <rte_lcore.h>
#include <rte_cycles.h>
#include <rte_cpuflags.h>
#include <rte_malloc.h>
#include <rte_ethdev.h>
#include <rte_power_intrinsics.h>

#include "rte_power_pmd_mgmt.h"

#define EMPTYPOLL_MAX  512

/* store some internal state */
static struct pmd_conf_data {
	/** what do we support? */
	struct rte_cpu_intrinsics intrinsics_support;
	/** pre-calculated tsc diff for 1us */
	uint64_t tsc_per_us;
	/** how many rte_pause can we fit in a microsecond? */
	uint64_t pause_per_us;
} global_data;

/**
 * Possible power management states of an ethdev port.
 */
enum pmd_mgmt_state {
	/** Device power management is disabled. */
	PMD_MGMT_DISABLED = 0,
	/** Device power management is enabled. */
	PMD_MGMT_ENABLED
};

struct pmd_queue_cfg {
	volatile enum pmd_mgmt_state pwr_mgmt_state;
	/**< State of power management for this queue */
	enum rte_power_pmd_mgmt_type cb_mode;
	/**< Callback mode for this queue */
	const struct rte_eth_rxtx_callback *cur_cb;
	/**< Callback instance */
	uint64_t empty_poll_stats;
	/**< Number of empty polls */
} __rte_cache_aligned;

static struct pmd_queue_cfg port_cfg[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT];

static void
calc_tsc(void)
{
	const uint64_t hz = rte_get_timer_hz();
	const uint64_t tsc_per_us = hz / US_PER_S; /* 1us */

	global_data.tsc_per_us = tsc_per_us;

	/* only do this if we don't have tpause */
	if (!global_data.intrinsics_support.power_pause) {
		const uint64_t start = rte_rdtsc_precise();
		const uint32_t n_pauses = 10000;
		double us, us_per_pause;
		uint64_t end;
		unsigned int i;

		/* estimate number of rte_pause() calls per us*/
		for (i = 0; i < n_pauses; i++)
			rte_pause();

		end = rte_rdtsc_precise();
		us = (end - start) / (double)tsc_per_us;
		us_per_pause = us / n_pauses;

		global_data.pause_per_us = (uint64_t)(1.0 / us_per_pause);
	}
}

static uint16_t
clb_umwait(uint16_t port_id, uint16_t qidx, struct rte_mbuf **pkts __rte_unused,
		uint16_t nb_rx, uint16_t max_pkts __rte_unused,
		void *addr __rte_unused)
{

	struct pmd_queue_cfg *q_conf;

	q_conf = &port_cfg[port_id][qidx];

	if (unlikely(nb_rx == 0)) {
		q_conf->empty_poll_stats++;
		if (unlikely(q_conf->empty_poll_stats > EMPTYPOLL_MAX)) {
			struct rte_power_monitor_cond pmc;
			uint16_t ret;

			/* use monitoring condition to sleep */
			ret = rte_eth_get_monitor_addr(port_id, qidx,
					&pmc);
			if (ret == 0)
				rte_power_monitor(&pmc, UINT64_MAX);
		}
	} else
		q_conf->empty_poll_stats = 0;

	return nb_rx;
}

static uint16_t
clb_pause(uint16_t port_id, uint16_t qidx, struct rte_mbuf **pkts __rte_unused,
		uint16_t nb_rx, uint16_t max_pkts __rte_unused,
		void *addr __rte_unused)
{
	struct pmd_queue_cfg *q_conf;

	q_conf = &port_cfg[port_id][qidx];

	if (unlikely(nb_rx == 0)) {
		q_conf->empty_poll_stats++;
		/* sleep for 1 microsecond */
		if (unlikely(q_conf->empty_poll_stats > EMPTYPOLL_MAX)) {
			/* use tpause if we have it */
			if (global_data.intrinsics_support.power_pause) {
				const uint64_t cur = rte_rdtsc();
				const uint64_t wait_tsc =
						cur + global_data.tsc_per_us;
				rte_power_pause(wait_tsc);
			} else {
				uint64_t i;
				for (i = 0; i < global_data.pause_per_us; i++)
					rte_pause();
			}
		}
	} else
		q_conf->empty_poll_stats = 0;

	return nb_rx;
}

static uint16_t
clb_scale_freq(uint16_t port_id, uint16_t qidx,
		struct rte_mbuf **pkts __rte_unused, uint16_t nb_rx,
		uint16_t max_pkts __rte_unused, void *_  __rte_unused)
{
	struct pmd_queue_cfg *q_conf;

	q_conf = &port_cfg[port_id][qidx];

	if (unlikely(nb_rx == 0)) {
		q_conf->empty_poll_stats++;
		if (unlikely(q_conf->empty_poll_stats > EMPTYPOLL_MAX))
			/* scale down freq */
			rte_power_freq_min(rte_lcore_id());
	} else {
		q_conf->empty_poll_stats = 0;
		/* scale up freq */
		rte_power_freq_max(rte_lcore_id());
	}

	return nb_rx;
}

static int
queue_stopped(const uint16_t port_id, const uint16_t queue_id)
{
	struct rte_eth_rxq_info qinfo;

	if (rte_eth_rx_queue_info_get(port_id, queue_id, &qinfo) < 0)
		return -1;

	return qinfo.queue_state == RTE_ETH_QUEUE_STATE_STOPPED;
}

int
rte_power_ethdev_pmgmt_queue_enable(unsigned int lcore_id, uint16_t port_id,
		uint16_t queue_id, enum rte_power_pmd_mgmt_type mode)
{
	struct pmd_queue_cfg *queue_cfg;
	struct rte_eth_dev_info info;
	rte_rx_callback_fn clb;
	int ret;

	RTE_ETH_VALID_PORTID_OR_ERR_RET(port_id, -EINVAL);

	if (queue_id >= RTE_MAX_QUEUES_PER_PORT || lcore_id >= RTE_MAX_LCORE) {
		ret = -EINVAL;
		goto end;
	}

	if (rte_eth_dev_info_get(port_id, &info) < 0) {
		ret = -EINVAL;
		goto end;
	}

	/* check if queue id is valid */
	if (queue_id >= info.nb_rx_queues) {
		ret = -EINVAL;
		goto end;
	}

	/* check if the queue is stopped */
	ret = queue_stopped(port_id, queue_id);
	if (ret != 1) {
		/* error means invalid queue, 0 means queue wasn't stopped */
		ret = ret < 0 ? -EINVAL : -EBUSY;
		goto end;
	}

	queue_cfg = &port_cfg[port_id][queue_id];

	if (queue_cfg->pwr_mgmt_state != PMD_MGMT_DISABLED) {
		ret = -EINVAL;
		goto end;
	}

	/* we need this in various places */
	rte_cpu_get_intrinsics_support(&global_data.intrinsics_support);

	switch (mode) {
	case RTE_POWER_MGMT_TYPE_MONITOR:
	{
		struct rte_power_monitor_cond dummy;

		/* check if rte_power_monitor is supported */
		if (!global_data.intrinsics_support.power_monitor) {
			RTE_LOG(DEBUG, POWER, "Monitoring intrinsics are not supported\n");
			ret = -ENOTSUP;
			goto end;
		}

		/* check if the device supports the necessary PMD API */
		if (rte_eth_get_monitor_addr(port_id, queue_id,
				&dummy) == -ENOTSUP) {
			RTE_LOG(DEBUG, POWER, "The device does not support rte_eth_get_monitor_addr\n");
			ret = -ENOTSUP;
			goto end;
		}
		clb = clb_umwait;
		break;
	}
	case RTE_POWER_MGMT_TYPE_SCALE:
	{
		enum power_management_env env;
		/* only PSTATE and ACPI modes are supported */
		if (!rte_power_check_env_supported(PM_ENV_ACPI_CPUFREQ) &&
				!rte_power_check_env_supported(
					PM_ENV_PSTATE_CPUFREQ)) {
			RTE_LOG(DEBUG, POWER, "Neither ACPI nor PSTATE modes are supported\n");
			ret = -ENOTSUP;
			goto end;
		}
		/* ensure we could initialize the power library */
		if (rte_power_init(lcore_id)) {
			ret = -EINVAL;
			goto end;
		}
		/* ensure we initialized the correct env */
		env = rte_power_get_env();
		if (env != PM_ENV_ACPI_CPUFREQ &&
				env != PM_ENV_PSTATE_CPUFREQ) {
			RTE_LOG(DEBUG, POWER, "Neither ACPI nor PSTATE modes were initialized\n");
			ret = -ENOTSUP;
			goto end;
		}
		clb = clb_scale_freq;
		break;
	}
	case RTE_POWER_MGMT_TYPE_PAUSE:
		/* figure out various time-to-tsc conversions */
		if (global_data.tsc_per_us == 0)
			calc_tsc();

		clb = clb_pause;
		break;
	default:
		RTE_LOG(DEBUG, POWER, "Invalid power management type\n");
		ret = -EINVAL;
		goto end;
	}

	/* initialize data before enabling the callback */
	queue_cfg->empty_poll_stats = 0;
	queue_cfg->cb_mode = mode;
	queue_cfg->pwr_mgmt_state = PMD_MGMT_ENABLED;
	queue_cfg->cur_cb = rte_eth_add_rx_callback(port_id, queue_id,
			clb, NULL);

	ret = 0;
end:
	return ret;
}

int
rte_power_ethdev_pmgmt_queue_disable(unsigned int lcore_id,
		uint16_t port_id, uint16_t queue_id)
{
	struct pmd_queue_cfg *queue_cfg;
	int ret;

	RTE_ETH_VALID_PORTID_OR_ERR_RET(port_id, -EINVAL);

	if (lcore_id >= RTE_MAX_LCORE || queue_id >= RTE_MAX_QUEUES_PER_PORT)
		return -EINVAL;

	/* check if the queue is stopped */
	ret = queue_stopped(port_id, queue_id);
	if (ret != 1) {
		/* error means invalid queue, 0 means queue wasn't stopped */
		return ret < 0 ? -EINVAL : -EBUSY;
	}

	/* no need to check queue id as wrong queue id would not be enabled */
	queue_cfg = &port_cfg[port_id][queue_id];

	if (queue_cfg->pwr_mgmt_state != PMD_MGMT_ENABLED)
		return -EINVAL;

	/* stop any callbacks from progressing */
	queue_cfg->pwr_mgmt_state = PMD_MGMT_DISABLED;

	switch (queue_cfg->cb_mode) {
	case RTE_POWER_MGMT_TYPE_MONITOR: /* fall-through */
	case RTE_POWER_MGMT_TYPE_PAUSE:
		rte_eth_remove_rx_callback(port_id, queue_id,
				queue_cfg->cur_cb);
		break;
	case RTE_POWER_MGMT_TYPE_SCALE:
		rte_power_freq_max(lcore_id);
		rte_eth_remove_rx_callback(port_id, queue_id,
				queue_cfg->cur_cb);
		rte_power_exit(lcore_id);
		break;
	}
	/*
	 * the API doc mandates that the user stops all processing on affected
	 * ports before calling any of these API's, so we can assume that the
	 * callbacks can be freed. we're intentionally casting away const-ness.
	 */
	rte_free((void *)queue_cfg->cur_cb);

	return 0;
}
