/*-
 * Copyright (c) 2014 Adrian Chadd <adrian@FreeBSD.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Driver for Intel WiFi Link 7260 series 802.11n/802.11ac adapters.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_wlan.h"
//#include "opt_iwa.h"

#include <sys/param.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/bus.h>
#include <sys/rman.h>
#include <sys/endian.h>
#include <sys/firmware.h>
#include <sys/limits.h>
#include <sys/module.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <machine/clock.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <net/bpf.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_arp.h>
#include <net/ethernet.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>
#include <net80211/ieee80211_regdomain.h>
#include <net80211/ieee80211_ratectl.h>

#include <dev/iwa/if_iwavar.h>

struct iwa_ident {
	uint16_t	vendor;
	uint16_t	device;
	const char	*name;
};

static const struct iwa_ident iwa_ident_table[] = {
#if 0
	{ 0x8086, 0x4060, "Intel Advanced 7260" },
#endif
	{ 0, 0, NULL }
};
static int
iwa_probe(device_t dev)
{
	const struct iwa_ident *ident;

	for (ident = iwa_ident_table; ident->name != NULL; ident++) {
		if (pci_get_vendor(dev) == ident->vendor &&
		    pci_get_device(dev) == ident->device) {
			device_set_desc(dev, ident->name);
			return (BUS_PROBE_DEFAULT);
		}
	}
	return (ENXIO);
}

static int
iwa_attach(device_t dev)
{
#if 0
	struct iwn_softc *sc = (struct iwn_softc *)device_get_softc(dev);
	struct ieee80211com *ic;
	struct ifnet *ifp;
	int i, error, rid;
	uint8_t macaddr[IEEE80211_ADDR_LEN];

	sc->sc_dev = dev;

#ifdef	IWN_DEBUG
	error = resource_int_value(device_get_name(sc->sc_dev),
	    device_get_unit(sc->sc_dev), "debug", &(sc->sc_debug));
	if (error != 0)
		sc->sc_debug = 0;
#else
	sc->sc_debug = 0;
#endif

	DPRINTF(sc, IWN_DEBUG_TRACE, "->%s: begin\n",__func__);

	/*
	 * Get the offset of the PCI Express Capability Structure in PCI
	 * Configuration Space.
	 */
	error = pci_find_cap(dev, PCIY_EXPRESS, &sc->sc_cap_off);
	if (error != 0) {
		device_printf(dev, "PCIe capability structure not found!\n");
		return error;
	}

	/* Clear device-specific "PCI retry timeout" register (41h). */
	pci_write_config(dev, 0x41, 0, 1);

	/* Enable bus-mastering. */
	pci_enable_busmaster(dev);

	rid = PCIR_BAR(0);
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->mem == NULL) {
		device_printf(dev, "can't map mem space\n");
		error = ENOMEM;
		return error;
	}
	sc->sc_st = rman_get_bustag(sc->mem);
	sc->sc_sh = rman_get_bushandle(sc->mem);

	i = 1;
	rid = 0;
	if (pci_alloc_msi(dev, &i) == 0)
		rid = 1;
	/* Install interrupt handler. */
	sc->irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid, RF_ACTIVE |
	    (rid != 0 ? 0 : RF_SHAREABLE));
	if (sc->irq == NULL) {
		device_printf(dev, "can't map interrupt\n");
		error = ENOMEM;
		goto fail;
	}

	IWN_LOCK_INIT(sc);

	/* Read hardware revision and attach. */
	sc->hw_type = (IWN_READ(sc, IWN_HW_REV) >> IWN_HW_REV_TYPE_SHIFT)
	    & IWN_HW_REV_TYPE_MASK;
	sc->subdevice_id = pci_get_subdevice(dev);

	/*
	 * 4965 versus 5000 and later have different methods.
	 * Let's set those up first.
	 */
	if (sc->hw_type == IWN_HW_REV_TYPE_4965)
		error = iwn4965_attach(sc, pci_get_device(dev));
	else
		error = iwn5000_attach(sc, pci_get_device(dev));
	if (error != 0) {
		device_printf(dev, "could not attach device, error %d\n",
		    error);
		goto fail;
	}

	/*
	 * Next, let's setup the various parameters of each NIC.
	 */
	error = iwn_config_specific(sc, pci_get_device(dev));
	if (error != 0) {
		device_printf(dev, "could not attach device, error %d\n",
		    error);
		goto fail;
	}

	if ((error = iwn_hw_prepare(sc)) != 0) {
		device_printf(dev, "hardware not ready, error %d\n", error);
		goto fail;
	}

	/* Allocate DMA memory for firmware transfers. */
	if ((error = iwn_alloc_fwmem(sc)) != 0) {
		device_printf(dev,
		    "could not allocate memory for firmware, error %d\n",
		    error);
		goto fail;
	}

	/* Allocate "Keep Warm" page. */
	if ((error = iwn_alloc_kw(sc)) != 0) {
		device_printf(dev,
		    "could not allocate keep warm page, error %d\n", error);
		goto fail;
	}

	/* Allocate ICT table for 5000 Series. */
	if (sc->hw_type != IWN_HW_REV_TYPE_4965 &&
	    (error = iwn_alloc_ict(sc)) != 0) {
		device_printf(dev, "could not allocate ICT table, error %d\n",
		    error);
		goto fail;
	}

	/* Allocate TX scheduler "rings". */
	if ((error = iwn_alloc_sched(sc)) != 0) {
		device_printf(dev,
		    "could not allocate TX scheduler rings, error %d\n", error);
		goto fail;
	}

	/* Allocate TX rings (16 on 4965AGN, 20 on >=5000). */
	for (i = 0; i < sc->ntxqs; i++) {
		if ((error = iwn_alloc_tx_ring(sc, &sc->txq[i], i)) != 0) {
			device_printf(dev,
			    "could not allocate TX ring %d, error %d\n", i,
			    error);
			goto fail;
		}
	}

	/* Allocate RX ring. */
	if ((error = iwn_alloc_rx_ring(sc, &sc->rxq)) != 0) {
		device_printf(dev, "could not allocate RX ring, error %d\n",
		    error);
		goto fail;
	}

	/* Clear pending interrupts. */
	IWN_WRITE(sc, IWN_INT, 0xffffffff);

	ifp = sc->sc_ifp = if_alloc(IFT_IEEE80211);
	if (ifp == NULL) {
		device_printf(dev, "can not allocate ifnet structure\n");
		goto fail;
	}

	ic = ifp->if_l2com;
	ic->ic_ifp = ifp;
	ic->ic_phytype = IEEE80211_T_OFDM;	/* not only, but not used */
	ic->ic_opmode = IEEE80211_M_STA;	/* default to BSS mode */

	/* Set device capabilities. */
	ic->ic_caps =
		  IEEE80211_C_STA		/* station mode supported */
		| IEEE80211_C_MONITOR		/* monitor mode supported */
		| IEEE80211_C_BGSCAN		/* background scanning */
		| IEEE80211_C_TXPMGT		/* tx power management */
		| IEEE80211_C_SHSLOT		/* short slot time supported */
		| IEEE80211_C_WPA
		| IEEE80211_C_SHPREAMBLE	/* short preamble supported */
#if 0
		| IEEE80211_C_IBSS		/* ibss/adhoc mode */
#endif
		| IEEE80211_C_WME		/* WME */
		| IEEE80211_C_PMGT		/* Station-side power mgmt */
		;

	/* Read MAC address, channels, etc from EEPROM. */
	if ((error = iwn_read_eeprom(sc, macaddr)) != 0) {
		device_printf(dev, "could not read EEPROM, error %d\n",
		    error);
		goto fail;
	}

	/* Count the number of available chains. */
	sc->ntxchains =
	    ((sc->txchainmask >> 2) & 1) +
	    ((sc->txchainmask >> 1) & 1) +
	    ((sc->txchainmask >> 0) & 1);
	sc->nrxchains =
	    ((sc->rxchainmask >> 2) & 1) +
	    ((sc->rxchainmask >> 1) & 1) +
	    ((sc->rxchainmask >> 0) & 1);
	if (bootverbose) {
		device_printf(dev, "MIMO %dT%dR, %.4s, address %6D\n",
		    sc->ntxchains, sc->nrxchains, sc->eeprom_domain,
		    macaddr, ":");
	}

	if (sc->sc_flags & IWN_FLAG_HAS_11N) {
		ic->ic_rxstream = sc->nrxchains;
		ic->ic_txstream = sc->ntxchains;

		/*
		 * Some of the 3 antenna devices (ie, the 4965) only supports
		 * 2x2 operation.  So correct the number of streams if
		 * it's not a 3-stream device.
		 */
		if (! iwn_is_3stream_device(sc)) {
			if (ic->ic_rxstream > 2)
				ic->ic_rxstream = 2;
			if (ic->ic_txstream > 2)
				ic->ic_txstream = 2;
		}

		ic->ic_htcaps =
			  IEEE80211_HTCAP_SMPS_OFF	/* SMPS mode disabled */
			| IEEE80211_HTCAP_SHORTGI20	/* short GI in 20MHz */
			| IEEE80211_HTCAP_CHWIDTH40	/* 40MHz channel width*/
			| IEEE80211_HTCAP_SHORTGI40	/* short GI in 40MHz */
#ifdef notyet
			| IEEE80211_HTCAP_GREENFIELD
#if IWN_RBUF_SIZE == 8192
			| IEEE80211_HTCAP_MAXAMSDU_7935	/* max A-MSDU length */
#else
			| IEEE80211_HTCAP_MAXAMSDU_3839	/* max A-MSDU length */
#endif
#endif
			/* s/w capabilities */
			| IEEE80211_HTC_HT		/* HT operation */
			| IEEE80211_HTC_AMPDU		/* tx A-MPDU */
#ifdef notyet
			| IEEE80211_HTC_AMSDU		/* tx A-MSDU */
#endif
			;
	}

	if_initname(ifp, device_get_name(dev), device_get_unit(dev));
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_init = iwn_init;
	ifp->if_ioctl = iwn_ioctl;
	ifp->if_start = iwn_start;
	IFQ_SET_MAXLEN(&ifp->if_snd, ifqmaxlen);
	ifp->if_snd.ifq_drv_maxlen = ifqmaxlen;
	IFQ_SET_READY(&ifp->if_snd);

	ieee80211_ifattach(ic, macaddr);
	ic->ic_vap_create = iwn_vap_create;
	ic->ic_vap_delete = iwn_vap_delete;
	ic->ic_raw_xmit = iwn_raw_xmit;
	ic->ic_node_alloc = iwn_node_alloc;
	sc->sc_ampdu_rx_start = ic->ic_ampdu_rx_start;
	ic->ic_ampdu_rx_start = iwn_ampdu_rx_start;
	sc->sc_ampdu_rx_stop = ic->ic_ampdu_rx_stop;
	ic->ic_ampdu_rx_stop = iwn_ampdu_rx_stop;
	sc->sc_addba_request = ic->ic_addba_request;
	ic->ic_addba_request = iwn_addba_request;
	sc->sc_addba_response = ic->ic_addba_response;
	ic->ic_addba_response = iwn_addba_response;
	sc->sc_addba_stop = ic->ic_addba_stop;
	ic->ic_addba_stop = iwn_ampdu_tx_stop;
	ic->ic_newassoc = iwn_newassoc;
	ic->ic_wme.wme_update = iwn_updateedca;
	ic->ic_update_mcast = iwn_update_mcast;
	ic->ic_scan_start = iwn_scan_start;
	ic->ic_scan_end = iwn_scan_end;
	ic->ic_set_channel = iwn_set_channel;
	ic->ic_scan_curchan = iwn_scan_curchan;
	ic->ic_scan_mindwell = iwn_scan_mindwell;
	ic->ic_setregdomain = iwn_setregdomain;

	iwn_radiotap_attach(sc);

	callout_init_mtx(&sc->calib_to, &sc->sc_mtx, 0);
	callout_init_mtx(&sc->watchdog_to, &sc->sc_mtx, 0);
	TASK_INIT(&sc->sc_reinit_task, 0, iwn_hw_reset, sc);
	TASK_INIT(&sc->sc_radioon_task, 0, iwn_radio_on, sc);
	TASK_INIT(&sc->sc_radiooff_task, 0, iwn_radio_off, sc);
	TASK_INIT(&sc->sc_panic_task, 0, iwn_panicked, sc);

	sc->sc_tq = taskqueue_create("iwn_taskq", M_WAITOK,
	    taskqueue_thread_enqueue, &sc->sc_tq);
	error = taskqueue_start_threads(&sc->sc_tq, 1, 0, "iwn_taskq");
	if (error != 0) {
		device_printf(dev, "can't start threads, error %d\n", error);
		goto fail;
	}

	iwn_sysctlattach(sc);

	/*
	 * Hook our interrupt after all initialization is complete.
	 */
	error = bus_setup_intr(dev, sc->irq, INTR_TYPE_NET | INTR_MPSAFE,
	    NULL, iwn_intr, sc, &sc->sc_ih);
	if (error != 0) {
		device_printf(dev, "can't establish interrupt, error %d\n",
		    error);
		goto fail;
	}

#if 0
	device_printf(sc->sc_dev, "%s: rx_stats=%d, rx_stats_bt=%d\n",
	    __func__,
	    sizeof(struct iwn_stats),
	    sizeof(struct iwn_stats_bt));
#endif

	if (bootverbose)
		ieee80211_announce(ic);
	DPRINTF(sc, IWN_DEBUG_TRACE, "->%s: end\n",__func__);
	return 0;
fail:
	iwn_detach(dev);
	DPRINTF(sc, IWN_DEBUG_TRACE, "->%s: end in error\n",__func__);
	return error;
#endif
	return (ENXIO);

}

static int
iwa_detach(device_t dev)
{
#if 0
	struct iwn_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic;
	int qid;

	DPRINTF(sc, IWN_DEBUG_TRACE, "->%s begin\n", __func__);

	if (ifp != NULL) {
		ic = ifp->if_l2com;

		ieee80211_draintask(ic, &sc->sc_reinit_task);
		ieee80211_draintask(ic, &sc->sc_radioon_task);
		ieee80211_draintask(ic, &sc->sc_radiooff_task);

		iwn_stop(sc);

		taskqueue_drain_all(sc->sc_tq);
		taskqueue_free(sc->sc_tq);

		callout_drain(&sc->watchdog_to);
		callout_drain(&sc->calib_to);
		ieee80211_ifdetach(ic);
	}

	/* Uninstall interrupt handler. */
	if (sc->irq != NULL) {
		bus_teardown_intr(dev, sc->irq, sc->sc_ih);
		bus_release_resource(dev, SYS_RES_IRQ, rman_get_rid(sc->irq),
		    sc->irq);
		pci_release_msi(dev);
	}

	/* Free DMA resources. */
	iwn_free_rx_ring(sc, &sc->rxq);
	for (qid = 0; qid < sc->ntxqs; qid++)
		iwn_free_tx_ring(sc, &sc->txq[qid]);
	iwn_free_sched(sc);
	iwn_free_kw(sc);
	if (sc->ict != NULL)
		iwn_free_ict(sc);
	iwn_free_fwmem(sc);

	if (sc->mem != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY,
		    rman_get_rid(sc->mem), sc->mem);

	if (ifp != NULL)
		if_free(ifp);

	DPRINTF(sc, IWN_DEBUG_TRACE, "->%s: end\n", __func__);
	IWN_LOCK_DESTROY(sc);
#endif
	return 0;
}

static int
iwa_shutdown(device_t dev)
{
#if 0
	struct iwn_softc *sc = device_get_softc(dev);

	iwn_stop(sc);
#endif
	return 0;
}

static int
iwa_suspend(device_t dev)
{
#if 0
	struct iwn_softc *sc = device_get_softc(dev);
	struct ieee80211com *ic = sc->sc_ifp->if_l2com;

	ieee80211_suspend_all(ic);
#endif
	return 0;
}

static int
iwa_resume(device_t dev)
{
#if 0
	struct iwn_softc *sc = device_get_softc(dev);
	struct ieee80211com *ic = sc->sc_ifp->if_l2com;

	/* Clear device-specific "PCI retry timeout" register (41h). */
	pci_write_config(dev, 0x41, 0, 1);

	ieee80211_resume_all(ic);
#endif
	return 0;
}

static device_method_t iwa_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		iwa_probe),
	DEVMETHOD(device_attach,	iwa_attach),
	DEVMETHOD(device_detach,	iwa_detach),
	DEVMETHOD(device_shutdown,	iwa_shutdown),
	DEVMETHOD(device_suspend,	iwa_suspend),
	DEVMETHOD(device_resume,	iwa_resume),

	DEVMETHOD_END
};

static driver_t iwa_driver = {
	"iwa",
	iwa_methods,
	sizeof(struct iwa_softc)
};
static devclass_t iwa_devclass;

DRIVER_MODULE(iwa, pci, iwa_driver, iwa_devclass, NULL, NULL);

MODULE_VERSION(iwa, 1);

MODULE_DEPEND(iwa, firmware, 1, 1, 1);
MODULE_DEPEND(iwa, pci, 1, 1, 1);
MODULE_DEPEND(iwa, wlan, 1, 1, 1);