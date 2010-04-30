/*-
 * Copyright (c) 2007-2009
 *	Damien Bergamini <damien.bergamini@free.fr>
 * Copyright (c) 2008
 *	Benjamin Close <benjsc@FreeBSD.org>
 * Copyright (c) 2008 Sam Leffler, Errno Consulting
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
 * Driver for Intel WiFi Link 4965 and 1000/5000/6000 Series 802.11 network
 * adapters.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

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
#include <net80211/ieee80211_amrr.h>
#include <net80211/ieee80211_radiotap.h>
#include <net80211/ieee80211_regdomain.h>

#include <dev/iwn/if_iwnreg.h>
#include <dev/iwn/if_iwnvar.h>

static int	iwn_probe(device_t);
static int	iwn_attach(device_t);
static const struct iwn_hal *iwn_hal_attach(struct iwn_softc *);
static void	iwn_radiotap_attach(struct iwn_softc *);
static struct ieee80211vap *iwn_vap_create(struct ieee80211com *,
		    const char name[IFNAMSIZ], int unit, int opmode,
		    int flags, const uint8_t bssid[IEEE80211_ADDR_LEN],
		    const uint8_t mac[IEEE80211_ADDR_LEN]);
static void	iwn_vap_delete(struct ieee80211vap *);
static int	iwn_cleanup(device_t);
static int	iwn_detach(device_t);
static int	iwn_nic_lock(struct iwn_softc *);
static int	iwn_eeprom_lock(struct iwn_softc *);
static int	iwn_init_otprom(struct iwn_softc *);
static int	iwn_read_prom_data(struct iwn_softc *, uint32_t, void *, int);
static void	iwn_dma_map_addr(void *, bus_dma_segment_t *, int, int);
static int	iwn_dma_contig_alloc(struct iwn_softc *, struct iwn_dma_info *,
		    void **, bus_size_t, bus_size_t, int);
static void	iwn_dma_contig_free(struct iwn_dma_info *);
static int	iwn_alloc_sched(struct iwn_softc *);
static void	iwn_free_sched(struct iwn_softc *);
static int	iwn_alloc_kw(struct iwn_softc *);
static void	iwn_free_kw(struct iwn_softc *);
static int	iwn_alloc_ict(struct iwn_softc *);
static void	iwn_free_ict(struct iwn_softc *);
static int	iwn_alloc_fwmem(struct iwn_softc *);
static void	iwn_free_fwmem(struct iwn_softc *);
static int	iwn_alloc_rx_ring(struct iwn_softc *, struct iwn_rx_ring *);
static void	iwn_reset_rx_ring(struct iwn_softc *, struct iwn_rx_ring *);
static void	iwn_free_rx_ring(struct iwn_softc *, struct iwn_rx_ring *);
static int	iwn_alloc_tx_ring(struct iwn_softc *, struct iwn_tx_ring *,
		    int);
static void	iwn_reset_tx_ring(struct iwn_softc *, struct iwn_tx_ring *);
static void	iwn_free_tx_ring(struct iwn_softc *, struct iwn_tx_ring *);
static void	iwn5000_ict_reset(struct iwn_softc *);
static int	iwn_read_eeprom(struct iwn_softc *,
		    uint8_t macaddr[IEEE80211_ADDR_LEN]);
static void	iwn4965_read_eeprom(struct iwn_softc *);
static void	iwn4965_print_power_group(struct iwn_softc *, int);
static void	iwn5000_read_eeprom(struct iwn_softc *);
static uint32_t	iwn_eeprom_channel_flags(struct iwn_eeprom_chan *);
static void	iwn_read_eeprom_band(struct iwn_softc *, int);
#if 0	/* HT */
static void	iwn_read_eeprom_ht40(struct iwn_softc *, int);
#endif
static void	iwn_read_eeprom_channels(struct iwn_softc *, int,
		    uint32_t);
static void	iwn_read_eeprom_enhinfo(struct iwn_softc *);
static struct ieee80211_node *iwn_node_alloc(struct ieee80211vap *,
		    const uint8_t mac[IEEE80211_ADDR_LEN]);
static void	iwn_newassoc(struct ieee80211_node *, int);
static int	iwn_media_change(struct ifnet *);
static int	iwn_newstate(struct ieee80211vap *, enum ieee80211_state, int);
static void	iwn_rx_phy(struct iwn_softc *, struct iwn_rx_desc *,
		    struct iwn_rx_data *);
static void	iwn_timer_timeout(void *);
static void	iwn_calib_reset(struct iwn_softc *);
static void	iwn_rx_done(struct iwn_softc *, struct iwn_rx_desc *,
		    struct iwn_rx_data *);
#if 0	/* HT */
static void	iwn_rx_compressed_ba(struct iwn_softc *, struct iwn_rx_desc *,
		    struct iwn_rx_data *);
#endif
static void	iwn5000_rx_calib_results(struct iwn_softc *,
		    struct iwn_rx_desc *, struct iwn_rx_data *);
static void	iwn_rx_statistics(struct iwn_softc *, struct iwn_rx_desc *,
		    struct iwn_rx_data *);
static void	iwn4965_tx_done(struct iwn_softc *, struct iwn_rx_desc *,
		    struct iwn_rx_data *);
static void	iwn5000_tx_done(struct iwn_softc *, struct iwn_rx_desc *,
		    struct iwn_rx_data *);
static void	iwn_tx_done(struct iwn_softc *, struct iwn_rx_desc *, int,
		    uint8_t);
static void	iwn_cmd_done(struct iwn_softc *, struct iwn_rx_desc *);
static void	iwn_notif_intr(struct iwn_softc *);
static void	iwn_wakeup_intr(struct iwn_softc *);
static void	iwn_rftoggle_intr(struct iwn_softc *);
static void	iwn_fatal_intr(struct iwn_softc *);
static void	iwn_intr(void *);
static void	iwn4965_update_sched(struct iwn_softc *, int, int, uint8_t,
		    uint16_t);
static void	iwn5000_update_sched(struct iwn_softc *, int, int, uint8_t,
		    uint16_t);
#ifdef notyet
static void	iwn5000_reset_sched(struct iwn_softc *, int, int);
#endif
static uint8_t	iwn_plcp_signal(int);
static int	iwn_tx_data(struct iwn_softc *, struct mbuf *,
		    struct ieee80211_node *, struct iwn_tx_ring *);
static int	iwn_raw_xmit(struct ieee80211_node *, struct mbuf *,
		    const struct ieee80211_bpf_params *);
static void	iwn_start(struct ifnet *);
static void	iwn_start_locked(struct ifnet *);
static void	iwn_watchdog(struct iwn_softc *sc);
static int	iwn_ioctl(struct ifnet *, u_long, caddr_t);
static int	iwn_cmd(struct iwn_softc *, int, const void *, int, int);
static int	iwn4965_add_node(struct iwn_softc *, struct iwn_node_info *,
		    int);
static int	iwn5000_add_node(struct iwn_softc *, struct iwn_node_info *,
		    int);
static int	iwn_set_link_quality(struct iwn_softc *, uint8_t, int);
static int	iwn_add_broadcast_node(struct iwn_softc *, int);
static int	iwn_wme_update(struct ieee80211com *);
static void	iwn_update_mcast(struct ifnet *);
static void	iwn_set_led(struct iwn_softc *, uint8_t, uint8_t, uint8_t);
static int	iwn_set_critical_temp(struct iwn_softc *);
static int	iwn_set_timing(struct iwn_softc *, struct ieee80211_node *);
static void	iwn4965_power_calibration(struct iwn_softc *, int);
static int	iwn4965_set_txpower(struct iwn_softc *,
		    struct ieee80211_channel *, int);
static int	iwn5000_set_txpower(struct iwn_softc *,
		    struct ieee80211_channel *, int);
static int	iwn4965_get_rssi(struct iwn_softc *, struct iwn_rx_stat *);
static int	iwn5000_get_rssi(struct iwn_softc *, struct iwn_rx_stat *);
static int	iwn_get_noise(const struct iwn_rx_general_stats *);
static int	iwn4965_get_temperature(struct iwn_softc *);
static int	iwn5000_get_temperature(struct iwn_softc *);
static int	iwn_init_sensitivity(struct iwn_softc *);
static void	iwn_collect_noise(struct iwn_softc *,
		    const struct iwn_rx_general_stats *);
static int	iwn4965_init_gains(struct iwn_softc *);
static int	iwn5000_init_gains(struct iwn_softc *);
static int	iwn4965_set_gains(struct iwn_softc *);
static int	iwn5000_set_gains(struct iwn_softc *);
static void	iwn_tune_sensitivity(struct iwn_softc *,
		    const struct iwn_rx_stats *);
static int	iwn_send_sensitivity(struct iwn_softc *);
static int	iwn_set_pslevel(struct iwn_softc *, int, int, int);
static int	iwn_config(struct iwn_softc *);
static int	iwn_scan(struct iwn_softc *);
static int	iwn_auth(struct iwn_softc *, struct ieee80211vap *vap);
static int	iwn_run(struct iwn_softc *, struct ieee80211vap *vap);
#if 0	/* HT */
static int	iwn_ampdu_rx_start(struct ieee80211com *,
		    struct ieee80211_node *, uint8_t);
static void	iwn_ampdu_rx_stop(struct ieee80211com *,
		    struct ieee80211_node *, uint8_t);
static int	iwn_ampdu_tx_start(struct ieee80211com *,
		    struct ieee80211_node *, uint8_t);
static void	iwn_ampdu_tx_stop(struct ieee80211com *,
		    struct ieee80211_node *, uint8_t);
static void	iwn4965_ampdu_tx_start(struct iwn_softc *,
		    struct ieee80211_node *, uint8_t, uint16_t);
static void	iwn4965_ampdu_tx_stop(struct iwn_softc *, uint8_t, uint16_t);
static void	iwn5000_ampdu_tx_start(struct iwn_softc *,
		    struct ieee80211_node *, uint8_t, uint16_t);
static void	iwn5000_ampdu_tx_stop(struct iwn_softc *, uint8_t, uint16_t);
#endif
static int	iwn5000_query_calibration(struct iwn_softc *);
static int	iwn5000_send_calibration(struct iwn_softc *);
static int	iwn5000_send_wimax_coex(struct iwn_softc *);
static int	iwn4965_post_alive(struct iwn_softc *);
static int	iwn5000_post_alive(struct iwn_softc *);
static int	iwn4965_load_bootcode(struct iwn_softc *, const uint8_t *,
		    int);
static int	iwn4965_load_firmware(struct iwn_softc *);
static int	iwn5000_load_firmware_section(struct iwn_softc *, uint32_t,
		    const uint8_t *, int);
static int	iwn5000_load_firmware(struct iwn_softc *);
static int	iwn_read_firmware(struct iwn_softc *);
static int	iwn_clock_wait(struct iwn_softc *);
static int	iwn_apm_init(struct iwn_softc *);
static void	iwn_apm_stop_master(struct iwn_softc *);
static void	iwn_apm_stop(struct iwn_softc *);
static int	iwn4965_nic_config(struct iwn_softc *);
static int	iwn5000_nic_config(struct iwn_softc *);
static int	iwn_hw_prepare(struct iwn_softc *);
static int	iwn_hw_init(struct iwn_softc *);
static void	iwn_hw_stop(struct iwn_softc *);
static void	iwn_init_locked(struct iwn_softc *);
static void	iwn_init(void *);
static void	iwn_stop_locked(struct iwn_softc *);
static void	iwn_stop(struct iwn_softc *);
static void 	iwn_scan_start(struct ieee80211com *);
static void 	iwn_scan_end(struct ieee80211com *);
static void 	iwn_set_channel(struct ieee80211com *);
static void 	iwn_scan_curchan(struct ieee80211_scan_state *, unsigned long);
static void 	iwn_scan_mindwell(struct ieee80211_scan_state *);
static struct iwn_eeprom_chan *iwn_find_eeprom_channel(struct iwn_softc *,
		    struct ieee80211_channel *);
static int	iwn_setregdomain(struct ieee80211com *,
		    struct ieee80211_regdomain *, int,
		    struct ieee80211_channel []);
static void	iwn_hw_reset(void *, int);
static void	iwn_radio_on(void *, int);
static void	iwn_radio_off(void *, int);
static void	iwn_sysctlattach(struct iwn_softc *);
static int	iwn_shutdown(device_t);
static int	iwn_suspend(device_t);
static int	iwn_resume(device_t);

#define IWN_DEBUG
#ifdef IWN_DEBUG
enum {
	IWN_DEBUG_XMIT		= 0x00000001,	/* basic xmit operation */
	IWN_DEBUG_RECV		= 0x00000002,	/* basic recv operation */
	IWN_DEBUG_STATE		= 0x00000004,	/* 802.11 state transitions */
	IWN_DEBUG_TXPOW		= 0x00000008,	/* tx power processing */
	IWN_DEBUG_RESET		= 0x00000010,	/* reset processing */
	IWN_DEBUG_OPS		= 0x00000020,	/* iwn_ops processing */
	IWN_DEBUG_BEACON 	= 0x00000040,	/* beacon handling */
	IWN_DEBUG_WATCHDOG 	= 0x00000080,	/* watchdog timeout */
	IWN_DEBUG_INTR		= 0x00000100,	/* ISR */
	IWN_DEBUG_CALIBRATE	= 0x00000200,	/* periodic calibration */
	IWN_DEBUG_NODE		= 0x00000400,	/* node management */
	IWN_DEBUG_LED		= 0x00000800,	/* led management */
	IWN_DEBUG_CMD		= 0x00001000,	/* cmd submission */
	IWN_DEBUG_FATAL		= 0x80000000,	/* fatal errors */
	IWN_DEBUG_ANY		= 0xffffffff
};

#define DPRINTF(sc, m, fmt, ...) do {			\
	if (sc->sc_debug & (m))				\
		printf(fmt, __VA_ARGS__);		\
} while (0)

static const char *iwn_intr_str(uint8_t);
#else
#define DPRINTF(sc, m, fmt, ...) do { (void) sc; } while (0)
#endif

struct iwn_ident {
	uint16_t	vendor;
	uint16_t	device;
	const char	*name;
};

static const struct iwn_ident iwn_ident_table [] = {
	{ 0x8086, 0x4229, "Intel(R) PRO/Wireless 4965BGN" },
	{ 0x8086, 0x422D, "Intel(R) PRO/Wireless 4965BGN" },
	{ 0x8086, 0x4230, "Intel(R) PRO/Wireless 4965BGN" },
	{ 0x8086, 0x4233, "Intel(R) PRO/Wireless 4965BGN" },
	{ 0x8086, 0x4232, "Intel(R) PRO/Wireless 5100" },
	{ 0x8086, 0x4237, "Intel(R) PRO/Wireless 5100" },
	{ 0x8086, 0x423C, "Intel(R) PRO/Wireless 5150" },
	{ 0x8086, 0x423D, "Intel(R) PRO/Wireless 5150" },
	{ 0x8086, 0x4235, "Intel(R) PRO/Wireless 5300" },
	{ 0x8086, 0x4236, "Intel(R) PRO/Wireless 5300" },
	{ 0x8086, 0x4236, "Intel(R) PRO/Wireless 5350" },
	{ 0x8086, 0x423A, "Intel(R) PRO/Wireless 5350" },
	{ 0x8086, 0x423B, "Intel(R) PRO/Wireless 5350" },
	{ 0x8086, 0x0083, "Intel(R) PRO/Wireless 1000" },
	{ 0x8086, 0x0084, "Intel(R) PRO/Wireless 1000" },
	{ 0x8086, 0x008D, "Intel(R) PRO/Wireless 6000" },
	{ 0x8086, 0x008E, "Intel(R) PRO/Wireless 6000" },
	{ 0x8086, 0x4238, "Intel(R) PRO/Wireless 6000" },
	{ 0x8086, 0x4239, "Intel(R) PRO/Wireless 6000" },
	{ 0x8086, 0x422B, "Intel(R) PRO/Wireless 6000" },
	{ 0x8086, 0x422C, "Intel(R) PRO/Wireless 6000" },
	{ 0x8086, 0x0086, "Intel(R) PRO/Wireless 6050" },
	{ 0x8086, 0x0087, "Intel(R) PRO/Wireless 6050" },
	{ 0, 0, NULL }
};

static const struct iwn_hal iwn4965_hal = {
	iwn4965_load_firmware,
	iwn4965_read_eeprom,
	iwn4965_post_alive,
	iwn4965_nic_config,
	iwn4965_update_sched,
	iwn4965_get_temperature,
	iwn4965_get_rssi,
	iwn4965_set_txpower,
	iwn4965_init_gains,
	iwn4965_set_gains,
	iwn4965_add_node,
	iwn4965_tx_done,
#if 0	/* HT */
	iwn4965_ampdu_tx_start,
	iwn4965_ampdu_tx_stop,
#endif
	IWN4965_NTXQUEUES,
	IWN4965_NDMACHNLS,
	IWN4965_ID_BROADCAST,
	IWN4965_RXONSZ,
	IWN4965_SCHEDSZ,
	IWN4965_FW_TEXT_MAXSZ,
	IWN4965_FW_DATA_MAXSZ,
	IWN4965_FWSZ,
	IWN4965_SCHED_TXFACT
};

static const struct iwn_hal iwn5000_hal = {
	iwn5000_load_firmware,
	iwn5000_read_eeprom,
	iwn5000_post_alive,
	iwn5000_nic_config,
	iwn5000_update_sched,
	iwn5000_get_temperature,
	iwn5000_get_rssi,
	iwn5000_set_txpower,
	iwn5000_init_gains,
	iwn5000_set_gains,
	iwn5000_add_node,
	iwn5000_tx_done,
#if 0	/* HT */
	iwn5000_ampdu_tx_start,
	iwn5000_ampdu_tx_stop,
#endif
	IWN5000_NTXQUEUES,
	IWN5000_NDMACHNLS,
	IWN5000_ID_BROADCAST,
	IWN5000_RXONSZ,
	IWN5000_SCHEDSZ,
	IWN5000_FW_TEXT_MAXSZ,
	IWN5000_FW_DATA_MAXSZ,
	IWN5000_FWSZ,
	IWN5000_SCHED_TXFACT
};

static int
iwn_probe(device_t dev)
{
	const struct iwn_ident *ident;

	for (ident = iwn_ident_table; ident->name != NULL; ident++) {
		if (pci_get_vendor(dev) == ident->vendor &&
		    pci_get_device(dev) == ident->device) {
			device_set_desc(dev, ident->name);
			return 0;
		}
	}
	return ENXIO;
}

static int
iwn_attach(device_t dev)
{
	struct iwn_softc *sc = (struct iwn_softc *)device_get_softc(dev);
	struct ieee80211com *ic;
	struct ifnet *ifp;
	const struct iwn_hal *hal;
	uint32_t tmp;
	int i, error, result;
	uint8_t macaddr[IEEE80211_ADDR_LEN];

	sc->sc_dev = dev;

	/*
	 * Get the offset of the PCI Express Capability Structure in PCI
	 * Configuration Space.
	 */
	error = pci_find_extcap(dev, PCIY_EXPRESS, &sc->sc_cap_off);
	if (error != 0) {
		device_printf(dev, "PCIe capability structure not found!\n");
		return error;
	}

	/* Clear device-specific "PCI retry timeout" register (41h). */
	pci_write_config(dev, 0x41, 0, 1);

	/* Hardware bug workaround. */
	tmp = pci_read_config(dev, PCIR_COMMAND, 1);
	if (tmp & PCIM_CMD_INTxDIS) {
		DPRINTF(sc, IWN_DEBUG_RESET, "%s: PCIe INTx Disable set\n",
		    __func__);
		tmp &= ~PCIM_CMD_INTxDIS;
		pci_write_config(dev, PCIR_COMMAND, tmp, 1);
	}

	/* Enable bus-mastering. */
	pci_enable_busmaster(dev);

	sc->mem_rid = PCIR_BAR(0);
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &sc->mem_rid,
	    RF_ACTIVE);
	if (sc->mem == NULL ) {
		device_printf(dev, "could not allocate memory resources\n");
		error = ENOMEM;
		return error;
	}

	sc->sc_st = rman_get_bustag(sc->mem);
	sc->sc_sh = rman_get_bushandle(sc->mem);
	sc->irq_rid = 0;
	if ((result = pci_msi_count(dev)) == 1 &&
	    pci_alloc_msi(dev, &result) == 0)
		sc->irq_rid = 1;
	sc->irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &sc->irq_rid,
	    RF_ACTIVE | RF_SHAREABLE);
	if (sc->irq == NULL) {
		device_printf(dev, "could not allocate interrupt resource\n");
		error = ENOMEM;
		goto fail;
	}

	IWN_LOCK_INIT(sc);
	callout_init_mtx(&sc->sc_timer_to, &sc->sc_mtx, 0);
	TASK_INIT(&sc->sc_reinit_task, 0, iwn_hw_reset, sc );
	TASK_INIT(&sc->sc_radioon_task, 0, iwn_radio_on, sc );
	TASK_INIT(&sc->sc_radiooff_task, 0, iwn_radio_off, sc );

	/* Attach Hardware Abstraction Layer. */
	hal = iwn_hal_attach(sc);
	if (hal == NULL) {
		error = ENXIO;	/* XXX: Wrong error code? */
		goto fail;
	}

	error = iwn_hw_prepare(sc);
	if (error != 0) {
		device_printf(dev, "hardware not ready, error %d\n", error);
		goto fail;
	}

	/* Allocate DMA memory for firmware transfers. */
	error = iwn_alloc_fwmem(sc);
	if (error != 0) {
		device_printf(dev,
		    "could not allocate memory for firmware, error %d\n",
		    error);
		goto fail;
	}

	/* Allocate "Keep Warm" page. */
	error = iwn_alloc_kw(sc);
	if (error != 0) {
		device_printf(dev,
		    "could not allocate \"Keep Warm\" page, error %d\n", error);
		goto fail;
	}

	/* Allocate ICT table for 5000 Series. */
	if (sc->hw_type != IWN_HW_REV_TYPE_4965 &&
	    (error = iwn_alloc_ict(sc)) != 0) {
		device_printf(dev,
		    "%s: could not allocate ICT table, error %d\n",
		    __func__, error);
		goto fail;
	}

	/* Allocate TX scheduler "rings". */
	error = iwn_alloc_sched(sc);
	if (error != 0) {
		device_printf(dev,
		    "could not allocate TX scheduler rings, error %d\n",
		    error);
		goto fail;
	}

	/* Allocate TX rings (16 on 4965AGN, 20 on 5000). */
	for (i = 0; i < hal->ntxqs; i++) {
		error = iwn_alloc_tx_ring(sc, &sc->txq[i], i);
		if (error != 0) {
			device_printf(dev,
			    "could not allocate Tx ring %d, error %d\n",
			    i, error);
			goto fail;
		}
	}

	/* Allocate RX ring. */
	error = iwn_alloc_rx_ring(sc, &sc->rxq);
	if (error != 0 ){
		device_printf(dev,
		    "could not allocate Rx ring, error %d\n", error);
		goto fail;
	}

	/* Clear pending interrupts. */
	IWN_WRITE(sc, IWN_INT, 0xffffffff);

	/* Count the number of available chains. */
	sc->ntxchains =
	    ((sc->txchainmask >> 2) & 1) +
	    ((sc->txchainmask >> 1) & 1) +
	    ((sc->txchainmask >> 0) & 1);
	sc->nrxchains =
	    ((sc->rxchainmask >> 2) & 1) +
	    ((sc->rxchainmask >> 1) & 1) +
	    ((sc->rxchainmask >> 0) & 1);

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
		| IEEE80211_C_TXPMGT		/* tx power management */
		| IEEE80211_C_SHSLOT		/* short slot time supported */
		| IEEE80211_C_WPA
		| IEEE80211_C_SHPREAMBLE	/* short preamble supported */
		| IEEE80211_C_BGSCAN		/* background scanning */
#if 0
		| IEEE80211_C_IBSS		/* ibss/adhoc mode */
#endif
		| IEEE80211_C_WME		/* WME */
		;
#if 0	/* HT */
	/* XXX disable until HT channel setup works */
	ic->ic_htcaps =
		  IEEE80211_HTCAP_SMPS_ENA	/* SM PS mode enabled */
		| IEEE80211_HTCAP_CHWIDTH40	/* 40MHz channel width */
		| IEEE80211_HTCAP_SHORTGI20	/* short GI in 20MHz */
		| IEEE80211_HTCAP_SHORTGI40	/* short GI in 40MHz */
		| IEEE80211_HTCAP_RXSTBC_2STREAM/* 1-2 spatial streams */
		| IEEE80211_HTCAP_MAXAMSDU_3839	/* max A-MSDU length */
		/* s/w capabilities */
		| IEEE80211_HTC_HT		/* HT operation */
		| IEEE80211_HTC_AMPDU		/* tx A-MPDU */
		| IEEE80211_HTC_AMSDU		/* tx A-MSDU */
		;

	/* Set HT capabilities. */
	ic->ic_htcaps =
#if IWN_RBUF_SIZE == 8192
	    IEEE80211_HTCAP_AMSDU7935 |
#endif
	    IEEE80211_HTCAP_CBW20_40 |
	    IEEE80211_HTCAP_SGI20 |
	    IEEE80211_HTCAP_SGI40;
	if (sc->hw_type != IWN_HW_REV_TYPE_4965)
		ic->ic_htcaps |= IEEE80211_HTCAP_GF;
	if (sc->hw_type == IWN_HW_REV_TYPE_6050)
		ic->ic_htcaps |= IEEE80211_HTCAP_SMPS_DYN;
	else
		ic->ic_htcaps |= IEEE80211_HTCAP_SMPS_DIS;
#endif

	/* Read MAC address, channels, etc from EEPROM. */
	error = iwn_read_eeprom(sc, macaddr);
	if (error != 0) {
		device_printf(dev, "could not read EEPROM, error %d\n",
		    error);
		goto fail;
	}

	device_printf(sc->sc_dev, "MIMO %dT%dR, %.4s, address %6D\n",
	    sc->ntxchains, sc->nrxchains, sc->eeprom_domain,
	    macaddr, ":");

#if 0	/* HT */
	/* Set supported HT rates. */
	ic->ic_sup_mcs[0] = 0xff;
	if (sc->nrxchains > 1)
		ic->ic_sup_mcs[1] = 0xff;
	if (sc->nrxchains > 2)
		ic->ic_sup_mcs[2] = 0xff;
#endif

	if_initname(ifp, device_get_name(dev), device_get_unit(dev));
	ifp->if_softc = sc;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_init = iwn_init;
	ifp->if_ioctl = iwn_ioctl;
	ifp->if_start = iwn_start;
	IFQ_SET_MAXLEN(&ifp->if_snd, IFQ_MAXLEN);
	ifp->if_snd.ifq_drv_maxlen = IFQ_MAXLEN;
	IFQ_SET_READY(&ifp->if_snd);

	ieee80211_ifattach(ic, macaddr);
	ic->ic_vap_create = iwn_vap_create;
	ic->ic_vap_delete = iwn_vap_delete;
	ic->ic_raw_xmit = iwn_raw_xmit;
	ic->ic_node_alloc = iwn_node_alloc;
	ic->ic_newassoc = iwn_newassoc;
	ic->ic_wme.wme_update = iwn_wme_update;
	ic->ic_update_mcast = iwn_update_mcast;
	ic->ic_scan_start = iwn_scan_start;
	ic->ic_scan_end = iwn_scan_end;
	ic->ic_set_channel = iwn_set_channel;
	ic->ic_scan_curchan = iwn_scan_curchan;
	ic->ic_scan_mindwell = iwn_scan_mindwell;
	ic->ic_setregdomain = iwn_setregdomain;
#if 0	/* HT */
	ic->ic_ampdu_rx_start = iwn_ampdu_rx_start;
	ic->ic_ampdu_rx_stop = iwn_ampdu_rx_stop;
	ic->ic_ampdu_tx_start = iwn_ampdu_tx_start;
	ic->ic_ampdu_tx_stop = iwn_ampdu_tx_stop;
#endif

	iwn_radiotap_attach(sc);
	iwn_sysctlattach(sc);

	/*
	 * Hook our interrupt after all initialization is complete.
	 */
	error = bus_setup_intr(dev, sc->irq, INTR_TYPE_NET | INTR_MPSAFE,
	    NULL, iwn_intr, sc, &sc->sc_ih);
	if (error != 0) {
		device_printf(dev, "could not set up interrupt, error %d\n",
		    error);
		goto fail;
	}

	ieee80211_announce(ic);
	return 0;
fail:
	iwn_cleanup(dev);
	return error;
}

static const struct iwn_hal *
iwn_hal_attach(struct iwn_softc *sc)
{
	sc->hw_type = (IWN_READ(sc, IWN_HW_REV) >> 4) & 0xf;

	switch (sc->hw_type) {
	case IWN_HW_REV_TYPE_4965:
		sc->sc_hal = &iwn4965_hal;
		sc->limits = &iwn4965_sensitivity_limits;
		sc->fwname = "iwn4965fw";
		sc->txchainmask = IWN_ANT_AB;
		sc->rxchainmask = IWN_ANT_ABC;
		break;
	case IWN_HW_REV_TYPE_5100:
		sc->sc_hal = &iwn5000_hal;
		sc->limits = &iwn5000_sensitivity_limits;
		sc->fwname = "iwn5000fw";
		sc->txchainmask = IWN_ANT_B;
		sc->rxchainmask = IWN_ANT_AB;
		break;
	case IWN_HW_REV_TYPE_5150:
		sc->sc_hal = &iwn5000_hal;
		sc->limits = &iwn5150_sensitivity_limits;
		sc->fwname = "iwn5150fw";
		sc->txchainmask = IWN_ANT_A;
		sc->rxchainmask = IWN_ANT_AB;
		break;
	case IWN_HW_REV_TYPE_5300:
	case IWN_HW_REV_TYPE_5350:
		sc->sc_hal = &iwn5000_hal;
		sc->limits = &iwn5000_sensitivity_limits;
		sc->fwname = "iwn5000fw";
		sc->txchainmask = IWN_ANT_ABC;
		sc->rxchainmask = IWN_ANT_ABC;
		break;
	case IWN_HW_REV_TYPE_1000:
		sc->sc_hal = &iwn5000_hal;
		sc->limits = &iwn1000_sensitivity_limits;
		sc->fwname = "iwn1000fw";
		sc->txchainmask = IWN_ANT_A;
		sc->rxchainmask = IWN_ANT_AB;
		break;
	case IWN_HW_REV_TYPE_6000:
		sc->sc_hal = &iwn5000_hal;
		sc->limits = &iwn6000_sensitivity_limits;
		sc->fwname = "iwn6000fw";
		switch (pci_get_device(sc->sc_dev)) {
		case 0x422C:
		case 0x4239:
			sc->sc_flags |= IWN_FLAG_INTERNAL_PA;
			sc->txchainmask = IWN_ANT_BC;
			sc->rxchainmask = IWN_ANT_BC;
			break;
		default:
			sc->txchainmask = IWN_ANT_ABC;
			sc->rxchainmask = IWN_ANT_ABC;
			break;
		}
		break;
	case IWN_HW_REV_TYPE_6050:
		sc->sc_hal = &iwn5000_hal;
		sc->limits = &iwn6000_sensitivity_limits;
		sc->fwname = "iwn6000fw";
		sc->txchainmask = IWN_ANT_AB;
		sc->rxchainmask = IWN_ANT_AB;
		break;
	default:
		device_printf(sc->sc_dev, "adapter type %d not supported\n",
		    sc->hw_type);
		return NULL;
	}
	return sc->sc_hal;
}

/*
 * Attach the interface to 802.11 radiotap.
 */
static void
iwn_radiotap_attach(struct iwn_softc *sc)
{
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;

	ieee80211_radiotap_attach(ic,
	    &sc->sc_txtap.wt_ihdr, sizeof(sc->sc_txtap),
		IWN_TX_RADIOTAP_PRESENT,
	    &sc->sc_rxtap.wr_ihdr, sizeof(sc->sc_rxtap),
		IWN_RX_RADIOTAP_PRESENT);
}

static struct ieee80211vap *
iwn_vap_create(struct ieee80211com *ic,
	const char name[IFNAMSIZ], int unit, int opmode, int flags,
	const uint8_t bssid[IEEE80211_ADDR_LEN],
	const uint8_t mac[IEEE80211_ADDR_LEN])
{
	struct iwn_vap *ivp;
	struct ieee80211vap *vap;

	if (!TAILQ_EMPTY(&ic->ic_vaps))		/* only one at a time */
		return NULL;
	ivp = (struct iwn_vap *) malloc(sizeof(struct iwn_vap),
	    M_80211_VAP, M_NOWAIT | M_ZERO);
	if (ivp == NULL)
		return NULL;
	vap = &ivp->iv_vap;
	ieee80211_vap_setup(ic, vap, name, unit, opmode, flags, bssid, mac);
	vap->iv_bmissthreshold = 10;		/* override default */
	/* Override with driver methods. */
	ivp->iv_newstate = vap->iv_newstate;
	vap->iv_newstate = iwn_newstate;

	ieee80211_amrr_init(&ivp->iv_amrr, vap,
	    IEEE80211_AMRR_MIN_SUCCESS_THRESHOLD,
	    IEEE80211_AMRR_MAX_SUCCESS_THRESHOLD,
	    500 /* ms */);

	/* Complete setup. */
	ieee80211_vap_attach(vap, iwn_media_change, ieee80211_media_status);
	ic->ic_opmode = opmode;
	return vap;
}

static void
iwn_vap_delete(struct ieee80211vap *vap)
{
	struct iwn_vap *ivp = IWN_VAP(vap);

	ieee80211_amrr_cleanup(&ivp->iv_amrr);
	ieee80211_vap_detach(vap);
	free(ivp, M_80211_VAP);
}

static int
iwn_cleanup(device_t dev)
{
	struct iwn_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic;
	int i;

	if (ifp != NULL) {
		ic = ifp->if_l2com;

		ieee80211_draintask(ic, &sc->sc_reinit_task);
		ieee80211_draintask(ic, &sc->sc_radioon_task);
		ieee80211_draintask(ic, &sc->sc_radiooff_task);

		iwn_stop(sc);
		callout_drain(&sc->sc_timer_to);
		ieee80211_ifdetach(ic);
	}

	/* Free DMA resources. */
	iwn_free_rx_ring(sc, &sc->rxq);
	if (sc->sc_hal != NULL)
		for (i = 0; i < sc->sc_hal->ntxqs; i++)
			iwn_free_tx_ring(sc, &sc->txq[i]);
	iwn_free_sched(sc);
	iwn_free_kw(sc);
	if (sc->ict != NULL)
		iwn_free_ict(sc);
	iwn_free_fwmem(sc);

	if (sc->irq != NULL) {
		bus_teardown_intr(dev, sc->irq, sc->sc_ih);
		bus_release_resource(dev, SYS_RES_IRQ, sc->irq_rid, sc->irq);
		if (sc->irq_rid == 1)
			pci_release_msi(dev);
	}

	if (sc->mem != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, sc->mem_rid, sc->mem);

	if (ifp != NULL)
		if_free(ifp);

	IWN_LOCK_DESTROY(sc);
	return 0;
}

static int
iwn_detach(device_t dev)
{
	iwn_cleanup(dev);
	return 0;
}

static int
iwn_nic_lock(struct iwn_softc *sc)
{
	int ntries;

	/* Request exclusive access to NIC. */
	IWN_SETBITS(sc, IWN_GP_CNTRL, IWN_GP_CNTRL_MAC_ACCESS_REQ);

	/* Spin until we actually get the lock. */
	for (ntries = 0; ntries < 1000; ntries++) {
		if ((IWN_READ(sc, IWN_GP_CNTRL) &
		    (IWN_GP_CNTRL_MAC_ACCESS_ENA | IWN_GP_CNTRL_SLEEP)) ==
		    IWN_GP_CNTRL_MAC_ACCESS_ENA)
			return 0;
		DELAY(10);
	}
	return ETIMEDOUT;
}

static __inline void
iwn_nic_unlock(struct iwn_softc *sc)
{
	IWN_CLRBITS(sc, IWN_GP_CNTRL, IWN_GP_CNTRL_MAC_ACCESS_REQ);
}

static __inline uint32_t
iwn_prph_read(struct iwn_softc *sc, uint32_t addr)
{
	IWN_WRITE(sc, IWN_PRPH_RADDR, IWN_PRPH_DWORD | addr);
	IWN_BARRIER_READ_WRITE(sc);
	return IWN_READ(sc, IWN_PRPH_RDATA);
}

static __inline void
iwn_prph_write(struct iwn_softc *sc, uint32_t addr, uint32_t data)
{
	IWN_WRITE(sc, IWN_PRPH_WADDR, IWN_PRPH_DWORD | addr);
	IWN_BARRIER_WRITE(sc);
	IWN_WRITE(sc, IWN_PRPH_WDATA, data);
}

static __inline void
iwn_prph_setbits(struct iwn_softc *sc, uint32_t addr, uint32_t mask)
{
	iwn_prph_write(sc, addr, iwn_prph_read(sc, addr) | mask);
}

static __inline void
iwn_prph_clrbits(struct iwn_softc *sc, uint32_t addr, uint32_t mask)
{
	iwn_prph_write(sc, addr, iwn_prph_read(sc, addr) & ~mask);
}

static __inline void
iwn_prph_write_region_4(struct iwn_softc *sc, uint32_t addr,
    const uint32_t *data, int count)
{
	for (; count > 0; count--, data++, addr += 4)
		iwn_prph_write(sc, addr, *data);
}

static __inline uint32_t
iwn_mem_read(struct iwn_softc *sc, uint32_t addr)
{
	IWN_WRITE(sc, IWN_MEM_RADDR, addr);
	IWN_BARRIER_READ_WRITE(sc);
	return IWN_READ(sc, IWN_MEM_RDATA);
}

static __inline void
iwn_mem_write(struct iwn_softc *sc, uint32_t addr, uint32_t data)
{
	IWN_WRITE(sc, IWN_MEM_WADDR, addr);
	IWN_BARRIER_WRITE(sc);
	IWN_WRITE(sc, IWN_MEM_WDATA, data);
}

static __inline void
iwn_mem_write_2(struct iwn_softc *sc, uint32_t addr, uint16_t data)
{
	uint32_t tmp;

	tmp = iwn_mem_read(sc, addr & ~3);
	if (addr & 3)
		tmp = (tmp & 0x0000ffff) | data << 16;
	else
		tmp = (tmp & 0xffff0000) | data;
	iwn_mem_write(sc, addr & ~3, tmp);
}

static __inline void
iwn_mem_read_region_4(struct iwn_softc *sc, uint32_t addr, uint32_t *data,
    int count)
{
	for (; count > 0; count--, addr += 4)
		*data++ = iwn_mem_read(sc, addr);
}

static __inline void
iwn_mem_set_region_4(struct iwn_softc *sc, uint32_t addr, uint32_t val,
    int count)
{
	for (; count > 0; count--, addr += 4)
		iwn_mem_write(sc, addr, val);
}

static int
iwn_eeprom_lock(struct iwn_softc *sc)
{
	int i, ntries;

	for (i = 0; i < 100; i++) {
		/* Request exclusive access to EEPROM. */
		IWN_SETBITS(sc, IWN_HW_IF_CONFIG,
		    IWN_HW_IF_CONFIG_EEPROM_LOCKED);

		/* Spin until we actually get the lock. */
		for (ntries = 0; ntries < 100; ntries++) {
			if (IWN_READ(sc, IWN_HW_IF_CONFIG) &
			    IWN_HW_IF_CONFIG_EEPROM_LOCKED)
				return 0;
			DELAY(10);
		}
	}
	return ETIMEDOUT;
}

static __inline void
iwn_eeprom_unlock(struct iwn_softc *sc)
{
	IWN_CLRBITS(sc, IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_EEPROM_LOCKED);
}

/*
 * Initialize access by host to One Time Programmable ROM.
 * NB: This kind of ROM can be found on 1000 or 6000 Series only.
 */
static int
iwn_init_otprom(struct iwn_softc *sc)
{
	uint16_t prev, base, next;
	int count, error;

	/* Wait for clock stabilization before accessing prph. */
	error = iwn_clock_wait(sc);
	if (error != 0)
		return error;

	error = iwn_nic_lock(sc);
	if (error != 0)
		return error;
	iwn_prph_setbits(sc, IWN_APMG_PS, IWN_APMG_PS_RESET_REQ);
	DELAY(5);
	iwn_prph_clrbits(sc, IWN_APMG_PS, IWN_APMG_PS_RESET_REQ);
	iwn_nic_unlock(sc);

	/* Set auto clock gate disable bit for HW with OTP shadow RAM. */
	if (sc->hw_type != IWN_HW_REV_TYPE_1000) {
		IWN_SETBITS(sc, IWN_DBG_LINK_PWR_MGMT,
		    IWN_RESET_LINK_PWR_MGMT_DIS);
	}
	IWN_CLRBITS(sc, IWN_EEPROM_GP, IWN_EEPROM_GP_IF_OWNER);
	/* Clear ECC status. */
	IWN_SETBITS(sc, IWN_OTP_GP,
	    IWN_OTP_GP_ECC_CORR_STTS | IWN_OTP_GP_ECC_UNCORR_STTS);

	/*
	 * Find the block before last block (contains the EEPROM image)
	 * for HW without OTP shadow RAM.
	 */
	if (sc->hw_type == IWN_HW_REV_TYPE_1000) {
		/* Switch to absolute addressing mode. */
		IWN_CLRBITS(sc, IWN_OTP_GP, IWN_OTP_GP_RELATIVE_ACCESS);
		base = prev = 0;
		for (count = 0; count < IWN1000_OTP_NBLOCKS; count++) {
			error = iwn_read_prom_data(sc, base, &next, 2);
			if (error != 0)
				return error;
			if (next == 0)	/* End of linked-list. */
				break;
			prev = base;
			base = le16toh(next);
		}
		if (count == 0 || count == IWN1000_OTP_NBLOCKS)
			return EIO;
		/* Skip "next" word. */
		sc->prom_base = prev + 1;
	}
	return 0;
}

static int
iwn_read_prom_data(struct iwn_softc *sc, uint32_t addr, void *data, int count)
{
	uint32_t val, tmp;
	int ntries;
	uint8_t *out = data;

	addr += sc->prom_base;
	for (; count > 0; count -= 2, addr++) {
		IWN_WRITE(sc, IWN_EEPROM, addr << 2);
		for (ntries = 0; ntries < 10; ntries++) {
			val = IWN_READ(sc, IWN_EEPROM);
			if (val & IWN_EEPROM_READ_VALID)
				break;
			DELAY(5);
		}
		if (ntries == 10) {
			device_printf(sc->sc_dev,
			    "timeout reading ROM at 0x%x\n", addr);
			return ETIMEDOUT;
		}
		if (sc->sc_flags & IWN_FLAG_HAS_OTPROM) {
			/* OTPROM, check for ECC errors. */
			tmp = IWN_READ(sc, IWN_OTP_GP);
			if (tmp & IWN_OTP_GP_ECC_UNCORR_STTS) {
				device_printf(sc->sc_dev,
				    "OTPROM ECC error at 0x%x\n", addr);
				return EIO;
			}
			if (tmp & IWN_OTP_GP_ECC_CORR_STTS) {
				/* Correctable ECC error, clear bit. */
				IWN_SETBITS(sc, IWN_OTP_GP,
				    IWN_OTP_GP_ECC_CORR_STTS);
			}
		}
		*out++ = val >> 16;
		if (count > 1)
			*out++ = val >> 24;
	}
	return 0;
}

static void
iwn_dma_map_addr(void *arg, bus_dma_segment_t *segs, int nsegs, int error)
{
	if (error != 0)
		return;
	KASSERT(nsegs == 1, ("too many DMA segments, %d should be 1", nsegs));
	*(bus_addr_t *)arg = segs[0].ds_addr;
}

static int
iwn_dma_contig_alloc(struct iwn_softc *sc, struct iwn_dma_info *dma,
	void **kvap, bus_size_t size, bus_size_t alignment, int flags)
{
	int error;

	dma->size = size;
	dma->tag = NULL;

	error = bus_dma_tag_create(bus_get_dma_tag(sc->sc_dev), alignment,
	    0, BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL, size,
	    1, size, flags, NULL, NULL, &dma->tag);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: bus_dma_tag_create failed, error %d\n",
		    __func__, error);
		goto fail;
	}
	error = bus_dmamem_alloc(dma->tag, (void **)&dma->vaddr,
	    flags | BUS_DMA_ZERO, &dma->map);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: bus_dmamem_alloc failed, error %d\n", __func__, error);
		goto fail;
	}
	error = bus_dmamap_load(dma->tag, dma->map, dma->vaddr,
	    size, iwn_dma_map_addr, &dma->paddr, flags);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: bus_dmamap_load failed, error %d\n", __func__, error);
		goto fail;
	}

	if (kvap != NULL)
		*kvap = dma->vaddr;
	return 0;
fail:
	iwn_dma_contig_free(dma);
	return error;
}

static void
iwn_dma_contig_free(struct iwn_dma_info *dma)
{
	if (dma->tag != NULL) {
		if (dma->map != NULL) {
			if (dma->paddr == 0) {
				bus_dmamap_sync(dma->tag, dma->map,
				    BUS_DMASYNC_POSTREAD|BUS_DMASYNC_POSTWRITE);
				bus_dmamap_unload(dma->tag, dma->map);
			}
			bus_dmamem_free(dma->tag, &dma->vaddr, dma->map);
		}
		bus_dma_tag_destroy(dma->tag);
	}
}

static int
iwn_alloc_sched(struct iwn_softc *sc)
{
	/* TX scheduler rings must be aligned on a 1KB boundary. */
	return iwn_dma_contig_alloc(sc, &sc->sched_dma,
	    (void **)&sc->sched, sc->sc_hal->schedsz, 1024, BUS_DMA_NOWAIT);
}

static void
iwn_free_sched(struct iwn_softc *sc)
{
	iwn_dma_contig_free(&sc->sched_dma);
}

static int
iwn_alloc_kw(struct iwn_softc *sc)
{
	/* "Keep Warm" page must be aligned on a 4KB boundary. */
	return iwn_dma_contig_alloc(sc, &sc->kw_dma, NULL, 4096, 4096,
	    BUS_DMA_NOWAIT);
}

static void
iwn_free_kw(struct iwn_softc *sc)
{
	iwn_dma_contig_free(&sc->kw_dma);
}

static int
iwn_alloc_ict(struct iwn_softc *sc)
{
	/* ICT table must be aligned on a 4KB boundary. */
	return iwn_dma_contig_alloc(sc, &sc->ict_dma,
	    (void **)&sc->ict, IWN_ICT_SIZE, 4096, BUS_DMA_NOWAIT);
}

static void
iwn_free_ict(struct iwn_softc *sc)
{
	iwn_dma_contig_free(&sc->ict_dma);
}

static int
iwn_alloc_fwmem(struct iwn_softc *sc)
{
	/* Must be aligned on a 16-byte boundary. */
	return iwn_dma_contig_alloc(sc, &sc->fw_dma, NULL,
	    sc->sc_hal->fwsz, 16, BUS_DMA_NOWAIT);
}

static void
iwn_free_fwmem(struct iwn_softc *sc)
{
	iwn_dma_contig_free(&sc->fw_dma);
}

static int
iwn_alloc_rx_ring(struct iwn_softc *sc, struct iwn_rx_ring *ring)
{
	bus_size_t size;
	int i, error;

	ring->cur = 0;

	/* Allocate RX descriptors (256-byte aligned). */
	size = IWN_RX_RING_COUNT * sizeof (uint32_t);
	error = iwn_dma_contig_alloc(sc, &ring->desc_dma,
	    (void **)&ring->desc, size, 256, BUS_DMA_NOWAIT);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not allocate Rx ring DMA memory, error %d\n",
		    __func__, error);
		goto fail;
	}

	error = bus_dma_tag_create(bus_get_dma_tag(sc->sc_dev), 1, 0,
	    BUS_SPACE_MAXADDR_32BIT,
	    BUS_SPACE_MAXADDR, NULL, NULL, MJUMPAGESIZE, 1,
	    MJUMPAGESIZE, BUS_DMA_NOWAIT, NULL, NULL, &ring->data_dmat);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: bus_dma_tag_create_failed, error %d\n",
		    __func__, error);
		goto fail;
	}

	/* Allocate RX status area (16-byte aligned). */
	error = iwn_dma_contig_alloc(sc, &ring->stat_dma,
	    (void **)&ring->stat, sizeof (struct iwn_rx_status),
	    16, BUS_DMA_NOWAIT);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not allocate Rx status DMA memory, error %d\n",
		    __func__, error);
		goto fail;
	}

	/*
	 * Allocate and map RX buffers.
	 */
	for (i = 0; i < IWN_RX_RING_COUNT; i++) {
		struct iwn_rx_data *data = &ring->data[i];
		bus_addr_t paddr;

		error = bus_dmamap_create(ring->data_dmat, 0, &data->map);
		if (error != 0) {
			device_printf(sc->sc_dev,
			    "%s: bus_dmamap_create failed, error %d\n",
			    __func__, error);
			goto fail;
		}

		data->m = m_getjcl(M_DONTWAIT, MT_DATA, M_PKTHDR, MJUMPAGESIZE);
		if (data->m == NULL) {
			device_printf(sc->sc_dev,
			    "%s: could not allocate rx mbuf\n", __func__);
			error = ENOMEM;
			goto fail;
		}

		/* Map page. */
		error = bus_dmamap_load(ring->data_dmat, data->map,
		    mtod(data->m, caddr_t), MJUMPAGESIZE,
		    iwn_dma_map_addr, &paddr, BUS_DMA_NOWAIT);
		if (error != 0 && error != EFBIG) {
			device_printf(sc->sc_dev,
			    "%s: bus_dmamap_load failed, error %d\n",
			    __func__, error);
			m_freem(data->m);
			error = ENOMEM;	/* XXX unique code */
			goto fail;
		}
		bus_dmamap_sync(ring->data_dmat, data->map,
		    BUS_DMASYNC_PREWRITE);

		/* Set physical address of RX buffer (256-byte aligned). */
		ring->desc[i] = htole32(paddr >> 8);
	}
	bus_dmamap_sync(ring->desc_dma.tag, ring->desc_dma.map,
	    BUS_DMASYNC_PREWRITE);
	return 0;
fail:
	iwn_free_rx_ring(sc, ring);
	return error;
}

static void
iwn_reset_rx_ring(struct iwn_softc *sc, struct iwn_rx_ring *ring)
{
	int ntries;

	if (iwn_nic_lock(sc) == 0) {
		IWN_WRITE(sc, IWN_FH_RX_CONFIG, 0);
		for (ntries = 0; ntries < 1000; ntries++) {
			if (IWN_READ(sc, IWN_FH_RX_STATUS) &
			    IWN_FH_RX_STATUS_IDLE)
				break;
			DELAY(10);
		}
		iwn_nic_unlock(sc);
#ifdef IWN_DEBUG
		if (ntries == 1000)
			DPRINTF(sc, IWN_DEBUG_ANY, "%s\n",
			    "timeout resetting Rx ring");
#endif
	}
	ring->cur = 0;
	sc->last_rx_valid = 0;
}

static void
iwn_free_rx_ring(struct iwn_softc *sc, struct iwn_rx_ring *ring)
{
	int i;

	iwn_dma_contig_free(&ring->desc_dma);
	iwn_dma_contig_free(&ring->stat_dma);

	for (i = 0; i < IWN_RX_RING_COUNT; i++) {
		struct iwn_rx_data *data = &ring->data[i];

		if (data->m != NULL) {
			bus_dmamap_sync(ring->data_dmat, data->map,
			    BUS_DMASYNC_POSTREAD);
			bus_dmamap_unload(ring->data_dmat, data->map);
			m_freem(data->m);
		}
		if (data->map != NULL)
			bus_dmamap_destroy(ring->data_dmat, data->map);
	}
}

static int
iwn_alloc_tx_ring(struct iwn_softc *sc, struct iwn_tx_ring *ring, int qid)
{
	bus_size_t size;
	bus_addr_t paddr;
	int i, error;

	ring->qid = qid;
	ring->queued = 0;
	ring->cur = 0;

	/* Allocate TX descriptors (256-byte aligned.) */
	size = IWN_TX_RING_COUNT * sizeof(struct iwn_tx_desc);
	error = iwn_dma_contig_alloc(sc, &ring->desc_dma,
	    (void **)&ring->desc, size, 256, BUS_DMA_NOWAIT);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not allocate TX ring DMA memory, error %d\n",
		    __func__, error);
		goto fail;
	}

	/*
	 * We only use rings 0 through 4 (4 EDCA + cmd) so there is no need
	 * to allocate commands space for other rings.
	 */
	if (qid > 4)
		return 0;

	size = IWN_TX_RING_COUNT * sizeof(struct iwn_tx_cmd);
	error = iwn_dma_contig_alloc(sc, &ring->cmd_dma,
	    (void **)&ring->cmd, size, 4, BUS_DMA_NOWAIT);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not allocate TX cmd DMA memory, error %d\n",
		    __func__, error);
		goto fail;
	}

	error = bus_dma_tag_create(bus_get_dma_tag(sc->sc_dev), 1, 0,
	    BUS_SPACE_MAXADDR_32BIT,
	    BUS_SPACE_MAXADDR, NULL, NULL, MCLBYTES, IWN_MAX_SCATTER - 1,
	    MCLBYTES, BUS_DMA_NOWAIT, NULL, NULL, &ring->data_dmat);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: bus_dma_tag_create_failed, error %d\n",
		    __func__, error);
		goto fail;
	}

	paddr = ring->cmd_dma.paddr;
	for (i = 0; i < IWN_TX_RING_COUNT; i++) {
		struct iwn_tx_data *data = &ring->data[i];

		data->cmd_paddr = paddr;
		data->scratch_paddr = paddr + 12;
		paddr += sizeof (struct iwn_tx_cmd);

		error = bus_dmamap_create(ring->data_dmat, 0, &data->map);
		if (error != 0) {
			device_printf(sc->sc_dev,
			    "%s: bus_dmamap_create failed, error %d\n",
			    __func__, error);
			goto fail;
		}
		bus_dmamap_sync(ring->data_dmat, data->map,
		    BUS_DMASYNC_PREWRITE);
	}
	return 0;
fail:
	iwn_free_tx_ring(sc, ring);
	return error;
}

static void
iwn_reset_tx_ring(struct iwn_softc *sc, struct iwn_tx_ring *ring)
{
	int i;

	for (i = 0; i < IWN_TX_RING_COUNT; i++) {
		struct iwn_tx_data *data = &ring->data[i];

		if (data->m != NULL) {
			bus_dmamap_unload(ring->data_dmat, data->map);
			m_freem(data->m);
			data->m = NULL;
		}
	}
	/* Clear TX descriptors. */
	memset(ring->desc, 0, ring->desc_dma.size);
	bus_dmamap_sync(ring->desc_dma.tag, ring->desc_dma.map,
	    BUS_DMASYNC_PREWRITE);
	sc->qfullmsk &= ~(1 << ring->qid);
	ring->queued = 0;
	ring->cur = 0;
}

static void
iwn_free_tx_ring(struct iwn_softc *sc, struct iwn_tx_ring *ring)
{
	int i;

	iwn_dma_contig_free(&ring->desc_dma);
	iwn_dma_contig_free(&ring->cmd_dma);

	for (i = 0; i < IWN_TX_RING_COUNT; i++) {
		struct iwn_tx_data *data = &ring->data[i];

		if (data->m != NULL) {
			bus_dmamap_sync(ring->data_dmat, data->map,
			    BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(ring->data_dmat, data->map);
			m_freem(data->m);
		}
		if (data->map != NULL)
			bus_dmamap_destroy(ring->data_dmat, data->map);
	}
}

static void
iwn5000_ict_reset(struct iwn_softc *sc)
{
	/* Disable interrupts. */
	IWN_WRITE(sc, IWN_INT_MASK, 0);

	/* Reset ICT table. */
	memset(sc->ict, 0, IWN_ICT_SIZE);
	sc->ict_cur = 0;

	/* Set physical address of ICT table (4KB aligned.) */
	DPRINTF(sc, IWN_DEBUG_RESET, "%s: enabling ICT\n", __func__);
	IWN_WRITE(sc, IWN_DRAM_INT_TBL, IWN_DRAM_INT_TBL_ENABLE |
	    IWN_DRAM_INT_TBL_WRAP_CHECK | sc->ict_dma.paddr >> 12);

	/* Enable periodic RX interrupt. */
	sc->int_mask |= IWN_INT_RX_PERIODIC;
	/* Switch to ICT interrupt mode in driver. */
	sc->sc_flags |= IWN_FLAG_USE_ICT;

	/* Re-enable interrupts. */
	IWN_WRITE(sc, IWN_INT, 0xffffffff);
	IWN_WRITE(sc, IWN_INT_MASK, sc->int_mask);
}

static int
iwn_read_eeprom(struct iwn_softc *sc, uint8_t macaddr[IEEE80211_ADDR_LEN])
{
	const struct iwn_hal *hal = sc->sc_hal;
	int error;
	uint16_t val;

	/* Check whether adapter has an EEPROM or an OTPROM. */
	if (sc->hw_type >= IWN_HW_REV_TYPE_1000 &&
	    (IWN_READ(sc, IWN_OTP_GP) & IWN_OTP_GP_DEV_SEL_OTP))
		sc->sc_flags |= IWN_FLAG_HAS_OTPROM;
	DPRINTF(sc, IWN_DEBUG_RESET, "%s found\n",
	    (sc->sc_flags & IWN_FLAG_HAS_OTPROM) ? "OTPROM" : "EEPROM");

	/* Adapter has to be powered on for EEPROM access to work. */
	error = iwn_apm_init(sc);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not power ON adapter, error %d\n",
		    __func__, error);
		return error;
	}

	if ((IWN_READ(sc, IWN_EEPROM_GP) & 0x7) == 0) {
		device_printf(sc->sc_dev, "%s: bad ROM signature\n", __func__);
		return EIO;
	}
	error = iwn_eeprom_lock(sc);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not lock ROM, error %d\n",
		    __func__, error);
		return error;
	}

	if (sc->sc_flags & IWN_FLAG_HAS_OTPROM) {
		error = iwn_init_otprom(sc);
		if (error != 0) {
			device_printf(sc->sc_dev,
			    "%s: could not initialize OTPROM, error %d\n",
			    __func__, error);
			return error;
		}
	}

	iwn_read_prom_data(sc, IWN_EEPROM_RFCFG, &val, 2);
	sc->rfcfg = le16toh(val);
	DPRINTF(sc, IWN_DEBUG_RESET, "radio config=0x%04x\n", sc->rfcfg);

	/* Read MAC address. */
	iwn_read_prom_data(sc, IWN_EEPROM_MAC, macaddr, 6);

	/* Read adapter-specific information from EEPROM. */
	hal->read_eeprom(sc);

	iwn_apm_stop(sc);	/* Power OFF adapter. */

	iwn_eeprom_unlock(sc);
	return 0;
}

static void
iwn4965_read_eeprom(struct iwn_softc *sc)
{
	uint32_t addr;
	int i;
	uint16_t val;

	/* Read regulatory domain (4 ASCII characters.) */
	iwn_read_prom_data(sc, IWN4965_EEPROM_DOMAIN, sc->eeprom_domain, 4);

	/* Read the list of authorized channels (20MHz ones only.) */
	for (i = 0; i < 5; i++) {
		addr = iwn4965_regulatory_bands[i];
		iwn_read_eeprom_channels(sc, i, addr);
	}

	/* Read maximum allowed TX power for 2GHz and 5GHz bands. */
	iwn_read_prom_data(sc, IWN4965_EEPROM_MAXPOW, &val, 2);
	sc->maxpwr2GHz = val & 0xff;
	sc->maxpwr5GHz = val >> 8;
	/* Check that EEPROM values are within valid range. */
	if (sc->maxpwr5GHz < 20 || sc->maxpwr5GHz > 50)
		sc->maxpwr5GHz = 38;
	if (sc->maxpwr2GHz < 20 || sc->maxpwr2GHz > 50)
		sc->maxpwr2GHz = 38;
	DPRINTF(sc, IWN_DEBUG_RESET, "maxpwr 2GHz=%d 5GHz=%d\n",
	    sc->maxpwr2GHz, sc->maxpwr5GHz);

	/* Read samples for each TX power group. */
	iwn_read_prom_data(sc, IWN4965_EEPROM_BANDS, sc->bands,
	    sizeof sc->bands);

	/* Read voltage at which samples were taken. */
	iwn_read_prom_data(sc, IWN4965_EEPROM_VOLTAGE, &val, 2);
	sc->eeprom_voltage = (int16_t)le16toh(val);
	DPRINTF(sc, IWN_DEBUG_RESET, "voltage=%d (in 0.3V)\n",
	    sc->eeprom_voltage);

#ifdef IWN_DEBUG
	/* Print samples. */
	if (sc->sc_debug & IWN_DEBUG_ANY) {
		for (i = 0; i < IWN_NBANDS; i++)
			iwn4965_print_power_group(sc, i);
	}
#endif
}

#ifdef IWN_DEBUG
static void
iwn4965_print_power_group(struct iwn_softc *sc, int i)
{
	struct iwn4965_eeprom_band *band = &sc->bands[i];
	struct iwn4965_eeprom_chan_samples *chans = band->chans;
	int j, c;

	printf("===band %d===\n", i);
	printf("chan lo=%d, chan hi=%d\n", band->lo, band->hi);
	printf("chan1 num=%d\n", chans[0].num);
	for (c = 0; c < 2; c++) {
		for (j = 0; j < IWN_NSAMPLES; j++) {
			printf("chain %d, sample %d: temp=%d gain=%d "
			    "power=%d pa_det=%d\n", c, j,
			    chans[0].samples[c][j].temp,
			    chans[0].samples[c][j].gain,
			    chans[0].samples[c][j].power,
			    chans[0].samples[c][j].pa_det);
		}
	}
	printf("chan2 num=%d\n", chans[1].num);
	for (c = 0; c < 2; c++) {
		for (j = 0; j < IWN_NSAMPLES; j++) {
			printf("chain %d, sample %d: temp=%d gain=%d "
			    "power=%d pa_det=%d\n", c, j,
			    chans[1].samples[c][j].temp,
			    chans[1].samples[c][j].gain,
			    chans[1].samples[c][j].power,
			    chans[1].samples[c][j].pa_det);
		}
	}
}
#endif

static void
iwn5000_read_eeprom(struct iwn_softc *sc)
{
	struct iwn5000_eeprom_calib_hdr hdr;
	int32_t temp, volt;
	uint32_t addr, base;
	int i;
	uint16_t val;

	/* Read regulatory domain (4 ASCII characters.) */
	iwn_read_prom_data(sc, IWN5000_EEPROM_REG, &val, 2);
	base = le16toh(val);
	iwn_read_prom_data(sc, base + IWN5000_EEPROM_DOMAIN,
	    sc->eeprom_domain, 4);

	/* Read the list of authorized channels (20MHz ones only.) */
	for (i = 0; i < 5; i++) {
		addr = base + iwn5000_regulatory_bands[i];
		iwn_read_eeprom_channels(sc, i, addr);
	}

	/* Read enhanced TX power information for 6000 Series. */
	if (sc->hw_type >= IWN_HW_REV_TYPE_6000)
		iwn_read_eeprom_enhinfo(sc);

	iwn_read_prom_data(sc, IWN5000_EEPROM_CAL, &val, 2);
	base = le16toh(val);
	iwn_read_prom_data(sc, base, &hdr, sizeof hdr);
	DPRINTF(sc, IWN_DEBUG_CALIBRATE,
	    "%s: calib version=%u pa type=%u voltage=%u\n",
	    __func__, hdr.version, hdr.pa_type, le16toh(hdr.volt));
	    sc->calib_ver = hdr.version;

	if (sc->hw_type == IWN_HW_REV_TYPE_5150) {
		/* Compute temperature offset. */
		iwn_read_prom_data(sc, base + IWN5000_EEPROM_TEMP, &val, 2);
		temp = le16toh(val);
		iwn_read_prom_data(sc, base + IWN5000_EEPROM_VOLT, &val, 2);
		volt = le16toh(val);
		sc->temp_off = temp - (volt / -5);
		DPRINTF(sc, IWN_DEBUG_CALIBRATE, "temp=%d volt=%d offset=%dK\n",
		    temp, volt, sc->temp_off);
	} else {
		/* Read crystal calibration. */
		iwn_read_prom_data(sc, base + IWN5000_EEPROM_CRYSTAL,
		    &sc->eeprom_crystal, sizeof (uint32_t));
		DPRINTF(sc, IWN_DEBUG_CALIBRATE, "crystal calibration 0x%08x\n",
		le32toh(sc->eeprom_crystal));
	}
}

/*
 * Translate EEPROM flags to net80211.
 */
static uint32_t
iwn_eeprom_channel_flags(struct iwn_eeprom_chan *channel)
{
	uint32_t nflags;

	nflags = 0;
	if ((channel->flags & IWN_EEPROM_CHAN_ACTIVE) == 0)
		nflags |= IEEE80211_CHAN_PASSIVE;
	if ((channel->flags & IWN_EEPROM_CHAN_IBSS) == 0)
		nflags |= IEEE80211_CHAN_NOADHOC;
	if (channel->flags & IWN_EEPROM_CHAN_RADAR) {
		nflags |= IEEE80211_CHAN_DFS;
		/* XXX apparently IBSS may still be marked */
		nflags |= IEEE80211_CHAN_NOADHOC;
	}

	return nflags;
}

static void
iwn_read_eeprom_band(struct iwn_softc *sc, int n)
{
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	struct iwn_eeprom_chan *channels = sc->eeprom_channels[n];
	const struct iwn_chan_band *band = &iwn_bands[n];
	struct ieee80211_channel *c;
	int i, chan, nflags;

	for (i = 0; i < band->nchan; i++) {
		if (!(channels[i].flags & IWN_EEPROM_CHAN_VALID)) {
			DPRINTF(sc, IWN_DEBUG_RESET,
			    "skip chan %d flags 0x%x maxpwr %d\n",
			    band->chan[i], channels[i].flags,
			    channels[i].maxpwr);
			continue;
		}
		chan = band->chan[i];
		nflags = iwn_eeprom_channel_flags(&channels[i]);

		DPRINTF(sc, IWN_DEBUG_RESET,
		    "add chan %d flags 0x%x maxpwr %d\n",
		    chan, channels[i].flags, channels[i].maxpwr);

		c = &ic->ic_channels[ic->ic_nchans++];
		c->ic_ieee = chan;
		c->ic_maxregpower = channels[i].maxpwr;
		c->ic_maxpower = 2*c->ic_maxregpower;

		/* Save maximum allowed TX power for this channel. */
		sc->maxpwr[chan] = channels[i].maxpwr;

		if (n == 0) {	/* 2GHz band */
			c->ic_freq = ieee80211_ieee2mhz(chan,
			    IEEE80211_CHAN_G);

			/* G =>'s B is supported */
			c->ic_flags = IEEE80211_CHAN_B | nflags;

			c = &ic->ic_channels[ic->ic_nchans++];
			c[0] = c[-1];
			c->ic_flags = IEEE80211_CHAN_G | nflags;
		} else {	/* 5GHz band */
			c->ic_freq = ieee80211_ieee2mhz(chan,
			    IEEE80211_CHAN_A);
			c->ic_flags = IEEE80211_CHAN_A | nflags;
			sc->sc_flags |= IWN_FLAG_HAS_5GHZ;
		}
#if 0	/* HT */
		/* XXX no constraints on using HT20 */
		/* add HT20, HT40 added separately */
		c = &ic->ic_channels[ic->ic_nchans++];
		c[0] = c[-1];
		c->ic_flags |= IEEE80211_CHAN_HT20;
		/* XXX NARROW =>'s 1/2 and 1/4 width? */
#endif
	}
}

#if 0	/* HT */
static void
iwn_read_eeprom_ht40(struct iwn_softc *sc, int n)
{
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	struct iwn_eeprom_chan *channels = sc->eeprom_channels[n];
	const struct iwn_chan_band *band = &iwn_bands[n];
	struct ieee80211_channel *c, *cent, *extc;
	int i;

	for (i = 0; i < band->nchan; i++) {
		if (!(channels[i].flags & IWN_EEPROM_CHAN_VALID) ||
		    !(channels[i].flags & IWN_EEPROM_CHAN_WIDE)) {
			DPRINTF(sc, IWN_DEBUG_RESET,
			    "skip chan %d flags 0x%x maxpwr %d\n",
			    band->chan[i], channels[i].flags,
			    channels[i].maxpwr);
			continue;
		}
		/*
		 * Each entry defines an HT40 channel pair; find the
		 * center channel, then the extension channel above.
		 */
		cent = ieee80211_find_channel_byieee(ic, band->chan[i],
		    band->flags & ~IEEE80211_CHAN_HT);
		if (cent == NULL) {	/* XXX shouldn't happen */
			device_printf(sc->sc_dev,
			    "%s: no entry for channel %d\n",
			    __func__, band->chan[i]);
			continue;
		}
		extc = ieee80211_find_channel(ic, cent->ic_freq+20,
		    band->flags & ~IEEE80211_CHAN_HT);
		if (extc == NULL) {
			DPRINTF(sc, IWN_DEBUG_RESET,
			    "skip chan %d, extension channel not found\n",
			    band->chan[i]);
			continue;
		}

		DPRINTF(sc, IWN_DEBUG_RESET,
		    "add ht40 chan %d flags 0x%x maxpwr %d\n",
		    band->chan[i], channels[i].flags, channels[i].maxpwr);

		c = &ic->ic_channels[ic->ic_nchans++];
		c[0] = cent[0];
		c->ic_extieee = extc->ic_ieee;
		c->ic_flags &= ~IEEE80211_CHAN_HT;
		c->ic_flags |= IEEE80211_CHAN_HT40U;
		c = &ic->ic_channels[ic->ic_nchans++];
		c[0] = extc[0];
		c->ic_extieee = cent->ic_ieee;
		c->ic_flags &= ~IEEE80211_CHAN_HT;
		c->ic_flags |= IEEE80211_CHAN_HT40D;
	}
}
#endif

static void
iwn_read_eeprom_channels(struct iwn_softc *sc, int n, uint32_t addr)
{
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;

	iwn_read_prom_data(sc, addr, &sc->eeprom_channels[n],
	    iwn_bands[n].nchan * sizeof (struct iwn_eeprom_chan));

	if (n < 5)
		iwn_read_eeprom_band(sc, n);
#if 0	/* HT */
	else
		iwn_read_eeprom_ht40(sc, n);
#endif
	ieee80211_sort_channels(ic->ic_channels, ic->ic_nchans);
}

#define nitems(_a)	(sizeof((_a)) / sizeof((_a)[0]))

static void
iwn_read_eeprom_enhinfo(struct iwn_softc *sc)
{
	struct iwn_eeprom_enhinfo enhinfo[35];
	uint16_t val, base;
	int8_t maxpwr;
	int i;

	iwn_read_prom_data(sc, IWN5000_EEPROM_REG, &val, 2);
	base = le16toh(val);
	iwn_read_prom_data(sc, base + IWN6000_EEPROM_ENHINFO,
	    enhinfo, sizeof enhinfo);

	memset(sc->enh_maxpwr, 0, sizeof sc->enh_maxpwr);
	for (i = 0; i < nitems(enhinfo); i++) {
		if (enhinfo[i].chan == 0 || enhinfo[i].reserved != 0)
			continue;	/* Skip invalid entries. */

		maxpwr = 0;
		if (sc->txchainmask & IWN_ANT_A)
			maxpwr = MAX(maxpwr, enhinfo[i].chain[0]);
		if (sc->txchainmask & IWN_ANT_B)
			maxpwr = MAX(maxpwr, enhinfo[i].chain[1]);
		if (sc->txchainmask & IWN_ANT_C)
			maxpwr = MAX(maxpwr, enhinfo[i].chain[2]);
		if (sc->ntxchains == 2)
			maxpwr = MAX(maxpwr, enhinfo[i].mimo2);
		else if (sc->ntxchains == 3)
			maxpwr = MAX(maxpwr, enhinfo[i].mimo3);
		maxpwr /= 2;	/* Convert half-dBm to dBm. */

		DPRINTF(sc, IWN_DEBUG_RESET, "enhinfo %d, maxpwr=%d\n", i,
		    maxpwr);
		sc->enh_maxpwr[i] = maxpwr;
	}
}

static struct ieee80211_node *
iwn_node_alloc(struct ieee80211vap *vap, const uint8_t mac[IEEE80211_ADDR_LEN])
{
	return malloc(sizeof (struct iwn_node), M_80211_NODE,M_NOWAIT | M_ZERO);
}

static void
iwn_newassoc(struct ieee80211_node *ni, int isnew)
{
	struct ieee80211vap *vap = ni->ni_vap;
	struct iwn_node *wn = (void *)ni;

	ieee80211_amrr_node_init(&IWN_VAP(vap)->iv_amrr,
	    &wn->amn, ni);
}

static int
iwn_media_change(struct ifnet *ifp)
{
	int error = ieee80211_media_change(ifp);
	/* NB: only the fixed rate can change and that doesn't need a reset */
	return (error == ENETRESET ? 0 : error);
}

static int
iwn_newstate(struct ieee80211vap *vap, enum ieee80211_state nstate, int arg)
{
	struct iwn_vap *ivp = IWN_VAP(vap);
	struct ieee80211com *ic = vap->iv_ic;
	struct iwn_softc *sc = ic->ic_ifp->if_softc;
	int error;

	DPRINTF(sc, IWN_DEBUG_STATE, "%s: %s -> %s\n", __func__,
		ieee80211_state_name[vap->iv_state],
		ieee80211_state_name[nstate]);

	IEEE80211_UNLOCK(ic);
	IWN_LOCK(sc);
	callout_stop(&sc->sc_timer_to);

	if (nstate == IEEE80211_S_AUTH && vap->iv_state != IEEE80211_S_AUTH) {
		/* !AUTH -> AUTH requires adapter config */
		/* Reset state to handle reassociations correctly. */
		sc->rxon.associd = 0;
		sc->rxon.filter &= ~htole32(IWN_FILTER_BSS);
		iwn_calib_reset(sc);
		error = iwn_auth(sc, vap);
	}
	if (nstate == IEEE80211_S_RUN && vap->iv_state != IEEE80211_S_RUN) {
		/*
		 * !RUN -> RUN requires setting the association id
		 * which is done with a firmware cmd.  We also defer
		 * starting the timers until that work is done.
		 */
		error = iwn_run(sc, vap);
	}
	if (nstate == IEEE80211_S_RUN) {
		/*
		 * RUN -> RUN transition; just restart the timers.
		 */
		iwn_calib_reset(sc);
	}
	IWN_UNLOCK(sc);
	IEEE80211_LOCK(ic);
	return ivp->iv_newstate(vap, nstate, arg);
}

/*
 * Process an RX_PHY firmware notification.  This is usually immediately
 * followed by an MPDU_RX_DONE notification.
 */
static void
iwn_rx_phy(struct iwn_softc *sc, struct iwn_rx_desc *desc,
    struct iwn_rx_data *data)
{
	struct iwn_rx_stat *stat = (struct iwn_rx_stat *)(desc + 1);

	DPRINTF(sc, IWN_DEBUG_CALIBRATE, "%s: received PHY stats\n", __func__);
	bus_dmamap_sync(sc->rxq.data_dmat, data->map, BUS_DMASYNC_POSTREAD);

	/* Save RX statistics, they will be used on MPDU_RX_DONE. */
	memcpy(&sc->last_rx_stat, stat, sizeof (*stat));
	sc->last_rx_valid = 1;
}

static void
iwn_timer_timeout(void *arg)
{
	struct iwn_softc *sc = arg;
	uint32_t flags = 0;

	IWN_LOCK_ASSERT(sc);

	if (sc->calib_cnt && --sc->calib_cnt == 0) {
		DPRINTF(sc, IWN_DEBUG_CALIBRATE, "%s\n",
		    "send statistics request");
		(void) iwn_cmd(sc, IWN_CMD_GET_STATISTICS, &flags,
		    sizeof flags, 1);
		sc->calib_cnt = 60;	/* do calibration every 60s */
	}
	iwn_watchdog(sc);		/* NB: piggyback tx watchdog */
	callout_reset(&sc->sc_timer_to, hz, iwn_timer_timeout, sc);
}

static void
iwn_calib_reset(struct iwn_softc *sc)
{
	callout_reset(&sc->sc_timer_to, hz, iwn_timer_timeout, sc);
	sc->calib_cnt = 60;		/* do calibration every 60s */
}

/*
 * Process an RX_DONE (4965AGN only) or MPDU_RX_DONE firmware notification.
 * Each MPDU_RX_DONE notification must be preceded by an RX_PHY one.
 */
static void
iwn_rx_done(struct iwn_softc *sc, struct iwn_rx_desc *desc,
    struct iwn_rx_data *data)
{
	const struct iwn_hal *hal = sc->sc_hal;
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	struct iwn_rx_ring *ring = &sc->rxq;
	struct ieee80211_frame *wh;
	struct ieee80211_node *ni;
	struct mbuf *m, *m1;
	struct iwn_rx_stat *stat;
	caddr_t head;
	bus_addr_t paddr;
	uint32_t flags;
	int error, len, rssi, nf;

	if (desc->type == IWN_MPDU_RX_DONE) {
		/* Check for prior RX_PHY notification. */
		if (!sc->last_rx_valid) {
			DPRINTF(sc, IWN_DEBUG_ANY,
			    "%s: missing RX_PHY\n", __func__);
			ifp->if_ierrors++;
			return;
		}
		sc->last_rx_valid = 0;
		stat = &sc->last_rx_stat;
	} else
		stat = (struct iwn_rx_stat *)(desc + 1);

	bus_dmamap_sync(ring->data_dmat, data->map, BUS_DMASYNC_POSTREAD);

	if (stat->cfg_phy_len > IWN_STAT_MAXLEN) {
		device_printf(sc->sc_dev,
		    "%s: invalid rx statistic header, len %d\n",
		    __func__, stat->cfg_phy_len);
		ifp->if_ierrors++;
		return;
	}
	if (desc->type == IWN_MPDU_RX_DONE) {
		struct iwn_rx_mpdu *mpdu = (struct iwn_rx_mpdu *)(desc + 1);
		head = (caddr_t)(mpdu + 1);
		len = le16toh(mpdu->len);
	} else {
		head = (caddr_t)(stat + 1) + stat->cfg_phy_len;
		len = le16toh(stat->len);
	}

	flags = le32toh(*(uint32_t *)(head + len));

	/* Discard frames with a bad FCS early. */
	if ((flags & IWN_RX_NOERROR) != IWN_RX_NOERROR) {
		DPRINTF(sc, IWN_DEBUG_RECV, "%s: rx flags error %x\n",
		    __func__, flags);
		ifp->if_ierrors++;
		return;
	}
	/* Discard frames that are too short. */
	if (len < sizeof (*wh)) {
		DPRINTF(sc, IWN_DEBUG_RECV, "%s: frame too short: %d\n",
		    __func__, len);
		ifp->if_ierrors++;
		return;
	}

	/* XXX don't need mbuf, just dma buffer */
	m1 = m_getjcl(M_DONTWAIT, MT_DATA, M_PKTHDR, MJUMPAGESIZE);
	if (m1 == NULL) {
		DPRINTF(sc, IWN_DEBUG_ANY, "%s: no mbuf to restock ring\n",
		    __func__);
		ifp->if_ierrors++;
		return;
	}
	bus_dmamap_unload(ring->data_dmat, data->map);

	error = bus_dmamap_load(ring->data_dmat, data->map,
	    mtod(m1, caddr_t), MJUMPAGESIZE,
	    iwn_dma_map_addr, &paddr, BUS_DMA_NOWAIT);
	if (error != 0 && error != EFBIG) {
		device_printf(sc->sc_dev,
		    "%s: bus_dmamap_load failed, error %d\n", __func__, error);
		m_freem(m1);
		ifp->if_ierrors++;
		return;
	}

	m = data->m;
	data->m = m1;
	/* Update RX descriptor. */
	ring->desc[ring->cur] = htole32(paddr >> 8);
	bus_dmamap_sync(ring->desc_dma.tag, ring->desc_dma.map,
	    BUS_DMASYNC_PREWRITE);

	/* Finalize mbuf. */
	m->m_pkthdr.rcvif = ifp;
	m->m_data = head;
	m->m_pkthdr.len = m->m_len = len;

	rssi = hal->get_rssi(sc, stat);

	/* Grab a reference to the source node. */
	wh = mtod(m, struct ieee80211_frame *);
	ni = ieee80211_find_rxnode(ic, (struct ieee80211_frame_min *)wh);
	nf = (ni != NULL && ni->ni_vap->iv_state == IEEE80211_S_RUN &&
	    (ic->ic_flags & IEEE80211_F_SCAN) == 0) ? sc->noise : -95;

	if (ieee80211_radiotap_active(ic)) {
		struct iwn_rx_radiotap_header *tap = &sc->sc_rxtap;

		tap->wr_tsft = htole64(stat->tstamp);
		tap->wr_flags = 0;
		if (stat->flags & htole16(IWN_STAT_FLAG_SHPREAMBLE))
			tap->wr_flags |= IEEE80211_RADIOTAP_F_SHORTPRE;
		switch (stat->rate) {
		/* CCK rates. */
		case  10: tap->wr_rate =   2; break;
		case  20: tap->wr_rate =   4; break;
		case  55: tap->wr_rate =  11; break;
		case 110: tap->wr_rate =  22; break;
		/* OFDM rates. */
		case 0xd: tap->wr_rate =  12; break;
		case 0xf: tap->wr_rate =  18; break;
		case 0x5: tap->wr_rate =  24; break;
		case 0x7: tap->wr_rate =  36; break;
		case 0x9: tap->wr_rate =  48; break;
		case 0xb: tap->wr_rate =  72; break;
		case 0x1: tap->wr_rate =  96; break;
		case 0x3: tap->wr_rate = 108; break;
		/* Unknown rate: should not happen. */
		default:  tap->wr_rate =   0;
		}
		tap->wr_dbm_antsignal = rssi;
		tap->wr_dbm_antnoise = nf;
	}

	IWN_UNLOCK(sc);

	/* Send the frame to the 802.11 layer. */
	if (ni != NULL) {
		(void) ieee80211_input(ni, m, rssi - nf, nf);
		/* Node is no longer needed. */
		ieee80211_free_node(ni);
	} else
		(void) ieee80211_input_all(ic, m, rssi - nf, nf);

	IWN_LOCK(sc);
}

#if 0	/* HT */
/* Process an incoming Compressed BlockAck. */
static void
iwn_rx_compressed_ba(struct iwn_softc *sc, struct iwn_rx_desc *desc,
    struct iwn_rx_data *data)
{
	struct iwn_compressed_ba *ba = (struct iwn_compressed_ba *)(desc + 1);
	struct iwn_tx_ring *txq;

	txq = &sc->txq[letoh16(ba->qid)];
	/* XXX TBD */
}
#endif

/*
 * Process a CALIBRATION_RESULT notification sent by the initialization
 * firmware on response to a CMD_CALIB_CONFIG command (5000 only.)
 */
static void
iwn5000_rx_calib_results(struct iwn_softc *sc, struct iwn_rx_desc *desc,
    struct iwn_rx_data *data)
{
	struct iwn_phy_calib *calib = (struct iwn_phy_calib *)(desc + 1);
	int len, idx = -1;

	/* Runtime firmware should not send such a notification. */
	if (sc->sc_flags & IWN_FLAG_CALIB_DONE)
		return;

	bus_dmamap_sync(sc->rxq.data_dmat, data->map, BUS_DMASYNC_POSTREAD);
	len = (le32toh(desc->len) & 0x3fff) - 4;

	switch (calib->code) {
	case IWN5000_PHY_CALIB_DC:
		if (sc->hw_type == IWN_HW_REV_TYPE_5150 ||
		    sc->hw_type == IWN_HW_REV_TYPE_6050)
			idx = 0;
		break;
	case IWN5000_PHY_CALIB_LO:
		idx = 1;
		break;
	case IWN5000_PHY_CALIB_TX_IQ:
		idx = 2;
		break;
	case IWN5000_PHY_CALIB_TX_IQ_PERIODIC:
		if (sc->hw_type < IWN_HW_REV_TYPE_6000 &&
		    sc->hw_type != IWN_HW_REV_TYPE_5150)
			idx = 3;
		break;
	case IWN5000_PHY_CALIB_BASE_BAND:
		idx = 4;
		break;
	}
	if (idx == -1)	/* Ignore other results. */
		return;

	/* Save calibration result. */
	if (sc->calibcmd[idx].buf != NULL)
		free(sc->calibcmd[idx].buf, M_DEVBUF);
	sc->calibcmd[idx].buf = malloc(len, M_DEVBUF, M_NOWAIT);
	if (sc->calibcmd[idx].buf == NULL) {
		DPRINTF(sc, IWN_DEBUG_CALIBRATE,
		    "not enough memory for calibration result %d\n",
		    calib->code);
		return;
	}
	DPRINTF(sc, IWN_DEBUG_CALIBRATE,
	    "saving calibration result code=%d len=%d\n", calib->code, len);
	sc->calibcmd[idx].len = len;
	memcpy(sc->calibcmd[idx].buf, calib, len);
}

/*
 * Process an RX_STATISTICS or BEACON_STATISTICS firmware notification.
 * The latter is sent by the firmware after each received beacon.
 */
static void
iwn_rx_statistics(struct iwn_softc *sc, struct iwn_rx_desc *desc,
    struct iwn_rx_data *data)
{
	const struct iwn_hal *hal = sc->sc_hal;
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	struct ieee80211vap *vap = TAILQ_FIRST(&ic->ic_vaps);
	struct iwn_calib_state *calib = &sc->calib;
	struct iwn_stats *stats = (struct iwn_stats *)(desc + 1);
	int temp;

	/* Beacon stats are meaningful only when associated and not scanning. */
	if (vap->iv_state != IEEE80211_S_RUN ||
	    (ic->ic_flags & IEEE80211_F_SCAN))
		return;

	bus_dmamap_sync(sc->rxq.data_dmat, data->map, BUS_DMASYNC_POSTREAD);
	DPRINTF(sc, IWN_DEBUG_CALIBRATE, "%s: cmd %d\n", __func__, desc->type);
	iwn_calib_reset(sc);	/* Reset TX power calibration timeout. */

	/* Test if temperature has changed. */
	if (stats->general.temp != sc->rawtemp) {
		/* Convert "raw" temperature to degC. */
		sc->rawtemp = stats->general.temp;
		temp = hal->get_temperature(sc);
		DPRINTF(sc, IWN_DEBUG_CALIBRATE, "%s: temperature %d\n",
		    __func__, temp);

		/* Update TX power if need be (4965AGN only.) */
		if (sc->hw_type == IWN_HW_REV_TYPE_4965)
			iwn4965_power_calibration(sc, temp);
	}

	if (desc->type != IWN_BEACON_STATISTICS)
		return;	/* Reply to a statistics request. */

	sc->noise = iwn_get_noise(&stats->rx.general);
	DPRINTF(sc, IWN_DEBUG_CALIBRATE, "%s: noise %d\n", __func__, sc->noise);

	/* Test that RSSI and noise are present in stats report. */
	if (le32toh(stats->rx.general.flags) != 1) {
		DPRINTF(sc, IWN_DEBUG_ANY, "%s\n",
		    "received statistics without RSSI");
		return;
	}

	if (calib->state == IWN_CALIB_STATE_ASSOC)
		iwn_collect_noise(sc, &stats->rx.general);
	else if (calib->state == IWN_CALIB_STATE_RUN)
		iwn_tune_sensitivity(sc, &stats->rx);
}

/*
 * Process a TX_DONE firmware notification.  Unfortunately, the 4965AGN
 * and 5000 adapters have different incompatible TX status formats.
 */
static void
iwn4965_tx_done(struct iwn_softc *sc, struct iwn_rx_desc *desc,
    struct iwn_rx_data *data)
{
	struct iwn4965_tx_stat *stat = (struct iwn4965_tx_stat *)(desc + 1);
	struct iwn_tx_ring *ring = &sc->txq[desc->qid & 0xf];

	DPRINTF(sc, IWN_DEBUG_XMIT, "%s: "
	    "qid %d idx %d retries %d nkill %d rate %x duration %d status %x\n",
	    __func__, desc->qid, desc->idx, stat->ackfailcnt,
	    stat->btkillcnt, stat->rate, le16toh(stat->duration),
	    le32toh(stat->status));

	bus_dmamap_sync(ring->data_dmat, data->map, BUS_DMASYNC_POSTREAD);
	iwn_tx_done(sc, desc, stat->ackfailcnt, le32toh(stat->status) & 0xff);
}

static void
iwn5000_tx_done(struct iwn_softc *sc, struct iwn_rx_desc *desc,
    struct iwn_rx_data *data)
{
	struct iwn5000_tx_stat *stat = (struct iwn5000_tx_stat *)(desc + 1);
	struct iwn_tx_ring *ring = &sc->txq[desc->qid & 0xf];

	DPRINTF(sc, IWN_DEBUG_XMIT, "%s: "
	    "qid %d idx %d retries %d nkill %d rate %x duration %d status %x\n",
	    __func__, desc->qid, desc->idx, stat->ackfailcnt,
	    stat->btkillcnt, stat->rate, le16toh(stat->duration),
	    le32toh(stat->status));

#ifdef notyet
	/* Reset TX scheduler slot. */
	iwn5000_reset_sched(sc, desc->qid & 0xf, desc->idx);
#endif

	bus_dmamap_sync(ring->data_dmat, data->map, BUS_DMASYNC_POSTREAD);
	iwn_tx_done(sc, desc, stat->ackfailcnt, le16toh(stat->status) & 0xff);
}

/*
 * Adapter-independent backend for TX_DONE firmware notifications.
 */
static void
iwn_tx_done(struct iwn_softc *sc, struct iwn_rx_desc *desc, int ackfailcnt,
    uint8_t status)
{
	struct ifnet *ifp = sc->sc_ifp;
	struct iwn_tx_ring *ring = &sc->txq[desc->qid & 0xf];
	struct iwn_tx_data *data = &ring->data[desc->idx];
	struct iwn_node *wn = (void *)data->ni;
	struct mbuf *m;
	struct ieee80211_node *ni;

	KASSERT(data->ni != NULL, ("no node"));

	/* Unmap and free mbuf. */
	bus_dmamap_sync(ring->data_dmat, data->map, BUS_DMASYNC_POSTWRITE);
	bus_dmamap_unload(ring->data_dmat, data->map);
	m = data->m, data->m = NULL;
	ni = data->ni, data->ni = NULL;

	if (m->m_flags & M_TXCB) {
		/*
		 * Channels marked for "radar" require traffic to be received
		 * to unlock before we can transmit.  Until traffic is seen
		 * any attempt to transmit is returned immediately with status
		 * set to IWN_TX_FAIL_TX_LOCKED.  Unfortunately this can easily
		 * happen on first authenticate after scanning.  To workaround
		 * this we ignore a failure of this sort in AUTH state so the
		 * 802.11 layer will fall back to using a timeout to wait for
		 * the AUTH reply.  This allows the firmware time to see
		 * traffic so a subsequent retry of AUTH succeeds.  It's
		 * unclear why the firmware does not maintain state for
		 * channels recently visited as this would allow immediate
		 * use of the channel after a scan (where we see traffic).
		 */
		if (status == IWN_TX_FAIL_TX_LOCKED &&
		    ni->ni_vap->iv_state == IEEE80211_S_AUTH)
			ieee80211_process_callback(ni, m, 0);
		else
			ieee80211_process_callback(ni, m,
			    (status & IWN_TX_FAIL) != 0);
	}

	/*
	 * Update rate control statistics for the node.
	 */
	if (status & 0x80) {
		ifp->if_oerrors++;
		ieee80211_amrr_tx_complete(&wn->amn,
		    IEEE80211_AMRR_FAILURE, ackfailcnt);
	} else {
		ieee80211_amrr_tx_complete(&wn->amn,
		    IEEE80211_AMRR_SUCCESS, ackfailcnt);
	}
	m_freem(m);
	ieee80211_free_node(ni);

	sc->sc_tx_timer = 0;
	if (--ring->queued < IWN_TX_RING_LOMARK) {
		sc->qfullmsk &= ~(1 << ring->qid);
		if (sc->qfullmsk == 0 &&
		    (ifp->if_drv_flags & IFF_DRV_OACTIVE)) {
			ifp->if_drv_flags &= ~IFF_DRV_OACTIVE;
			iwn_start_locked(ifp);
		}
	}
}

/*
 * Process a "command done" firmware notification.  This is where we wakeup
 * processes waiting for a synchronous command completion.
 */
static void
iwn_cmd_done(struct iwn_softc *sc, struct iwn_rx_desc *desc)
{
	struct iwn_tx_ring *ring = &sc->txq[4];
	struct iwn_tx_data *data;

	if ((desc->qid & 0xf) != 4)
		return;	/* Not a command ack. */

	data = &ring->data[desc->idx];

	/* If the command was mapped in an mbuf, free it. */
	if (data->m != NULL) {
		bus_dmamap_unload(ring->data_dmat, data->map);
		m_freem(data->m);
		data->m = NULL;
	}
	wakeup(&ring->desc[desc->idx]);
}

/*
 * Process an INT_FH_RX or INT_SW_RX interrupt.
 */
static void
iwn_notif_intr(struct iwn_softc *sc)
{
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	struct ieee80211vap *vap = TAILQ_FIRST(&ic->ic_vaps);
	uint16_t hw;

	bus_dmamap_sync(sc->rxq.stat_dma.tag, sc->rxq.stat_dma.map,
	    BUS_DMASYNC_POSTREAD);

	hw = le16toh(sc->rxq.stat->closed_count) & 0xfff;
	while (sc->rxq.cur != hw) {
		struct iwn_rx_data *data = &sc->rxq.data[sc->rxq.cur];
		struct iwn_rx_desc *desc;

		bus_dmamap_sync(sc->rxq.data_dmat, data->map,
		    BUS_DMASYNC_POSTREAD);
		desc = mtod(data->m, struct iwn_rx_desc *);

		DPRINTF(sc, IWN_DEBUG_RECV,
		    "%s: qid %x idx %d flags %x type %d(%s) len %d\n",
		    __func__, desc->qid & 0xf, desc->idx, desc->flags,
		    desc->type, iwn_intr_str(desc->type),
		    le16toh(desc->len));

		if (!(desc->qid & 0x80))	/* Reply to a command. */
			iwn_cmd_done(sc, desc);

		switch (desc->type) {
		case IWN_RX_PHY:
			iwn_rx_phy(sc, desc, data);
			break;

		case IWN_RX_DONE:		/* 4965AGN only. */
		case IWN_MPDU_RX_DONE:
			/* An 802.11 frame has been received. */
			iwn_rx_done(sc, desc, data);
			break;

#if 0	/* HT */
		case IWN_RX_COMPRESSED_BA:
			/* A Compressed BlockAck has been received. */
			iwn_rx_compressed_ba(sc, desc, data);
			break;
#endif

		case IWN_TX_DONE:
			/* An 802.11 frame has been transmitted. */
			sc->sc_hal->tx_done(sc, desc, data);
			break;

		case IWN_RX_STATISTICS:
		case IWN_BEACON_STATISTICS:
			iwn_rx_statistics(sc, desc, data);
			break;

		case IWN_BEACON_MISSED:
		{
			struct iwn_beacon_missed *miss =
			    (struct iwn_beacon_missed *)(desc + 1);
			int misses;

			bus_dmamap_sync(sc->rxq.data_dmat, data->map,
			    BUS_DMASYNC_POSTREAD);
			misses = le32toh(miss->consecutive);

			/* XXX not sure why we're notified w/ zero */
			if (misses == 0)
				break;
			DPRINTF(sc, IWN_DEBUG_STATE,
			    "%s: beacons missed %d/%d\n", __func__,
			    misses, le32toh(miss->total));

			/*
			 * If more than 5 consecutive beacons are missed,
			 * reinitialize the sensitivity state machine.
			 */
			if (vap->iv_state == IEEE80211_S_RUN && misses > 5)
				(void) iwn_init_sensitivity(sc);
			if (misses >= vap->iv_bmissthreshold) {
				IWN_UNLOCK(sc);
				ieee80211_beacon_miss(ic);
				IWN_LOCK(sc);
			}
			break;
		}
		case IWN_UC_READY:
		{
			struct iwn_ucode_info *uc =
			    (struct iwn_ucode_info *)(desc + 1);

			/* The microcontroller is ready. */
			bus_dmamap_sync(sc->rxq.data_dmat, data->map,
			    BUS_DMASYNC_POSTREAD);
			DPRINTF(sc, IWN_DEBUG_RESET,
			    "microcode alive notification version=%d.%d "
			    "subtype=%x alive=%x\n", uc->major, uc->minor,
			    uc->subtype, le32toh(uc->valid));

			if (le32toh(uc->valid) != 1) {
				device_printf(sc->sc_dev,
				    "microcontroller initialization failed");
				break;
			}
			if (uc->subtype == IWN_UCODE_INIT) {
				/* Save microcontroller report. */
				memcpy(&sc->ucode_info, uc, sizeof (*uc));
			}
			/* Save the address of the error log in SRAM. */
			sc->errptr = le32toh(uc->errptr);
			break;
		}
		case IWN_STATE_CHANGED:
		{
			uint32_t *status = (uint32_t *)(desc + 1);

			/*
			 * State change allows hardware switch change to be
			 * noted. However, we handle this in iwn_intr as we
			 * get both the enable/disble intr.
			 */
			bus_dmamap_sync(sc->rxq.data_dmat, data->map,
			    BUS_DMASYNC_POSTREAD);
			DPRINTF(sc, IWN_DEBUG_INTR, "state changed to %x\n",
			    le32toh(*status));
			break;
		}
		case IWN_START_SCAN:
		{
			struct iwn_start_scan *scan =
			    (struct iwn_start_scan *)(desc + 1);

			bus_dmamap_sync(sc->rxq.data_dmat, data->map,
			    BUS_DMASYNC_POSTREAD);
			DPRINTF(sc, IWN_DEBUG_ANY,
			    "%s: scanning channel %d status %x\n",
			    __func__, scan->chan, le32toh(scan->status));
			break;
		}
		case IWN_STOP_SCAN:
		{
			struct iwn_stop_scan *scan =
			    (struct iwn_stop_scan *)(desc + 1);

			bus_dmamap_sync(sc->rxq.data_dmat, data->map,
			    BUS_DMASYNC_POSTREAD);
			DPRINTF(sc, IWN_DEBUG_STATE,
			    "scan finished nchan=%d status=%d chan=%d\n",
			    scan->nchan, scan->status, scan->chan);

			IWN_UNLOCK(sc);
			ieee80211_scan_next(vap);
			IWN_LOCK(sc);
			break;
		}
		case IWN5000_CALIBRATION_RESULT:
			iwn5000_rx_calib_results(sc, desc, data);
			break;

		case IWN5000_CALIBRATION_DONE:
			sc->sc_flags |= IWN_FLAG_CALIB_DONE;
			wakeup(sc);
			break;
		}

		sc->rxq.cur = (sc->rxq.cur + 1) % IWN_RX_RING_COUNT;
	}

	/* Tell the firmware what we have processed. */
	hw = (hw == 0) ? IWN_RX_RING_COUNT - 1 : hw - 1;
	IWN_WRITE(sc, IWN_FH_RX_WPTR, hw & ~7);
}

/*
 * Process an INT_WAKEUP interrupt raised when the microcontroller wakes up
 * from power-down sleep mode.
 */
static void
iwn_wakeup_intr(struct iwn_softc *sc)
{
	int qid;

	DPRINTF(sc, IWN_DEBUG_RESET, "%s: ucode wakeup from power-down sleep\n",
	    __func__);

	/* Wakeup RX and TX rings. */
	IWN_WRITE(sc, IWN_FH_RX_WPTR, sc->rxq.cur & ~7);
	for (qid = 0; qid < sc->sc_hal->ntxqs; qid++) {
		struct iwn_tx_ring *ring = &sc->txq[qid];
		IWN_WRITE(sc, IWN_HBUS_TARG_WRPTR, qid << 8 | ring->cur);
	}
}

static void
iwn_rftoggle_intr(struct iwn_softc *sc)
{
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	uint32_t tmp = IWN_READ(sc, IWN_GP_CNTRL);

	IWN_LOCK_ASSERT(sc);

	device_printf(sc->sc_dev, "RF switch: radio %s\n",
	    (tmp & IWN_GP_CNTRL_RFKILL) ? "enabled" : "disabled");
	if (tmp & IWN_GP_CNTRL_RFKILL)
		ieee80211_runtask(ic, &sc->sc_radioon_task);
	else
		ieee80211_runtask(ic, &sc->sc_radiooff_task);
}

/*
 * Dump the error log of the firmware when a firmware panic occurs.  Although
 * we can't debug the firmware because it is neither open source nor free, it
 * can help us to identify certain classes of problems.
 */
static void
iwn_fatal_intr(struct iwn_softc *sc)
{
	const struct iwn_hal *hal = sc->sc_hal;
	struct iwn_fw_dump dump;
	int i;

	IWN_LOCK_ASSERT(sc);

	/* Force a complete recalibration on next init. */
	sc->sc_flags &= ~IWN_FLAG_CALIB_DONE;

	/* Check that the error log address is valid. */
	if (sc->errptr < IWN_FW_DATA_BASE ||
	    sc->errptr + sizeof (dump) >
	    IWN_FW_DATA_BASE + hal->fw_data_maxsz) {
		printf("%s: bad firmware error log address 0x%08x\n",
		    __func__, sc->errptr);
		return;
	}
	if (iwn_nic_lock(sc) != 0) {
		printf("%s: could not read firmware error log\n",
		    __func__);
		return;
	}
	/* Read firmware error log from SRAM. */
	iwn_mem_read_region_4(sc, sc->errptr, (uint32_t *)&dump,
	    sizeof (dump) / sizeof (uint32_t));
	iwn_nic_unlock(sc);

	if (dump.valid == 0) {
		printf("%s: firmware error log is empty\n",
		    __func__);
		return;
	}
	printf("firmware error log:\n");
	printf("  error type      = \"%s\" (0x%08X)\n",
	    (dump.id < nitems(iwn_fw_errmsg)) ?
		iwn_fw_errmsg[dump.id] : "UNKNOWN",
	    dump.id);
	printf("  program counter = 0x%08X\n", dump.pc);
	printf("  source line     = 0x%08X\n", dump.src_line);
	printf("  error data      = 0x%08X%08X\n",
	    dump.error_data[0], dump.error_data[1]);
	printf("  branch link     = 0x%08X%08X\n",
	    dump.branch_link[0], dump.branch_link[1]);
	printf("  interrupt link  = 0x%08X%08X\n",
	    dump.interrupt_link[0], dump.interrupt_link[1]);
	printf("  time            = %u\n", dump.time[0]);

	/* Dump driver status (TX and RX rings) while we're here. */
	printf("driver status:\n");
	for (i = 0; i < hal->ntxqs; i++) {
		struct iwn_tx_ring *ring = &sc->txq[i];
		printf("  tx ring %2d: qid=%-2d cur=%-3d queued=%-3d\n",
		    i, ring->qid, ring->cur, ring->queued);
	}
	printf("  rx ring: cur=%d\n", sc->rxq.cur);
}

static void
iwn_intr(void *arg)
{
	struct iwn_softc *sc = arg;
	struct ifnet *ifp = sc->sc_ifp;
	uint32_t r1, r2, tmp;

	IWN_LOCK(sc);

	/* Disable interrupts. */
	IWN_WRITE(sc, IWN_INT_MASK, 0);

	/* Read interrupts from ICT (fast) or from registers (slow). */
	if (sc->sc_flags & IWN_FLAG_USE_ICT) {
		tmp = 0;
		while (sc->ict[sc->ict_cur] != 0) {
			tmp |= sc->ict[sc->ict_cur];
			sc->ict[sc->ict_cur] = 0;	/* Acknowledge. */
			sc->ict_cur = (sc->ict_cur + 1) % IWN_ICT_COUNT;
		}
		tmp = le32toh(tmp);
		if (tmp == 0xffffffff)	/* Shouldn't happen. */
			tmp = 0;
		else if (tmp & 0xc0000)	/* Workaround a HW bug. */
			tmp |= 0x8000;
		r1 = (tmp & 0xff00) << 16 | (tmp & 0xff);
		r2 = 0;	/* Unused. */
	} else {
		r1 = IWN_READ(sc, IWN_INT);
		if (r1 == 0xffffffff || (r1 & 0xfffffff0) == 0xa5a5a5a0)
			return;	/* Hardware gone! */
		r2 = IWN_READ(sc, IWN_FH_INT);
	}

	DPRINTF(sc, IWN_DEBUG_INTR, "interrupt reg1=%x reg2=%x\n", r1, r2);

	if (r1 == 0 && r2 == 0)
		goto done;	/* Interrupt not for us. */

	/* Acknowledge interrupts. */
	IWN_WRITE(sc, IWN_INT, r1);
	if (!(sc->sc_flags & IWN_FLAG_USE_ICT))
		IWN_WRITE(sc, IWN_FH_INT, r2);

	if (r1 & IWN_INT_RF_TOGGLED) {
		iwn_rftoggle_intr(sc);
		goto done;
	}
	if (r1 & IWN_INT_CT_REACHED) {
		device_printf(sc->sc_dev, "%s: critical temperature reached!\n",
		    __func__);
	}
	if (r1 & (IWN_INT_SW_ERR | IWN_INT_HW_ERR)) {
		iwn_fatal_intr(sc);
		ifp->if_flags &= ~IFF_UP;
		iwn_stop_locked(sc);
		goto done;
	}
	if ((r1 & (IWN_INT_FH_RX | IWN_INT_SW_RX | IWN_INT_RX_PERIODIC)) ||
	    (r2 & IWN_FH_INT_RX)) {
		if (sc->sc_flags & IWN_FLAG_USE_ICT) {
			if (r1 & (IWN_INT_FH_RX | IWN_INT_SW_RX))
				IWN_WRITE(sc, IWN_FH_INT, IWN_FH_INT_RX);
			IWN_WRITE_1(sc, IWN_INT_PERIODIC,
			    IWN_INT_PERIODIC_DIS);
			iwn_notif_intr(sc);
			if (r1 & (IWN_INT_FH_RX | IWN_INT_SW_RX)) {
				IWN_WRITE_1(sc, IWN_INT_PERIODIC,
				    IWN_INT_PERIODIC_ENA);
			}
		} else
			iwn_notif_intr(sc);
	}

	if ((r1 & IWN_INT_FH_TX) || (r2 & IWN_FH_INT_TX)) {
		if (sc->sc_flags & IWN_FLAG_USE_ICT)
			IWN_WRITE(sc, IWN_FH_INT, IWN_FH_INT_TX);
		wakeup(sc);	/* FH DMA transfer completed. */
	}

	if (r1 & IWN_INT_ALIVE)
		wakeup(sc);	/* Firmware is alive. */

	if (r1 & IWN_INT_WAKEUP)
		iwn_wakeup_intr(sc);

done:
	/* Re-enable interrupts. */
	if (ifp->if_flags & IFF_UP)
		IWN_WRITE(sc, IWN_INT_MASK, sc->int_mask);

	IWN_UNLOCK(sc);
}

/*
 * Update TX scheduler ring when transmitting an 802.11 frame (4965AGN and
 * 5000 adapters use a slightly different format.)
 */
static void
iwn4965_update_sched(struct iwn_softc *sc, int qid, int idx, uint8_t id,
    uint16_t len)
{
	uint16_t *w = &sc->sched[qid * IWN4965_SCHED_COUNT + idx];

	*w = htole16(len + 8);
	bus_dmamap_sync(sc->sched_dma.tag, sc->sched_dma.map,
	    BUS_DMASYNC_PREWRITE);
	if (idx < IWN_SCHED_WINSZ) {
		*(w + IWN_TX_RING_COUNT) = *w;
		bus_dmamap_sync(sc->sched_dma.tag, sc->sched_dma.map,
		    BUS_DMASYNC_PREWRITE);
	}
}

static void
iwn5000_update_sched(struct iwn_softc *sc, int qid, int idx, uint8_t id,
    uint16_t len)
{
	uint16_t *w = &sc->sched[qid * IWN5000_SCHED_COUNT + idx];

	*w = htole16(id << 12 | (len + 8));

	bus_dmamap_sync(sc->sched_dma.tag, sc->sched_dma.map,
	    BUS_DMASYNC_PREWRITE);
	if (idx < IWN_SCHED_WINSZ) {
		*(w + IWN_TX_RING_COUNT) = *w;
		bus_dmamap_sync(sc->sched_dma.tag, sc->sched_dma.map,
		    BUS_DMASYNC_PREWRITE);
	}
}

#ifdef notyet
static void
iwn5000_reset_sched(struct iwn_softc *sc, int qid, int idx)
{
	uint16_t *w = &sc->sched[qid * IWN5000_SCHED_COUNT + idx];

	*w = (*w & htole16(0xf000)) | htole16(1);
	bus_dmamap_sync(sc->sched_dma.tag, sc->sched_dma.map,
	    BUS_DMASYNC_PREWRITE);
	if (idx < IWN_SCHED_WINSZ) {
		*(w + IWN_TX_RING_COUNT) = *w;
		bus_dmamap_sync(sc->sched_dma.tag, sc->sched_dma.map,
		    BUS_DMASYNC_PREWRITE);
	}
}
#endif

static uint8_t
iwn_plcp_signal(int rate) {
	int i;

	for (i = 0; i < IWN_RIDX_MAX + 1; i++) {
		if (rate == iwn_rates[i].rate)
			return i;
	}

	return 0;
}

static int
iwn_tx_data(struct iwn_softc *sc, struct mbuf *m, struct ieee80211_node *ni,
    struct iwn_tx_ring *ring)
{
	const struct iwn_hal *hal = sc->sc_hal;
	const struct ieee80211_txparam *tp;
	const struct iwn_rate *rinfo;
	struct ieee80211vap *vap = ni->ni_vap;
	struct ieee80211com *ic = ni->ni_ic;
	struct iwn_node *wn = (void *)ni;
	struct iwn_tx_desc *desc;
	struct iwn_tx_data *data;
	struct iwn_tx_cmd *cmd;
	struct iwn_cmd_data *tx;
	struct ieee80211_frame *wh;
	struct ieee80211_key *k = NULL;
	struct mbuf *mnew;
	bus_dma_segment_t segs[IWN_MAX_SCATTER];
	uint32_t flags;
	u_int hdrlen;
	int totlen, error, pad, nsegs = 0, i, rate;
	uint8_t ridx, type, txant;

	IWN_LOCK_ASSERT(sc);

	wh = mtod(m, struct ieee80211_frame *);
	hdrlen = ieee80211_anyhdrsize(wh);
	type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;

	desc = &ring->desc[ring->cur];
	data = &ring->data[ring->cur];

	/* Choose a TX rate index. */
	tp = &vap->iv_txparms[ieee80211_chan2mode(ni->ni_chan)];
	if (type == IEEE80211_FC0_TYPE_MGT)
		rate = tp->mgmtrate;
	else if (IEEE80211_IS_MULTICAST(wh->i_addr1))
		rate = tp->mcastrate;
	else if (tp->ucastrate != IEEE80211_FIXED_RATE_NONE)
		rate = tp->ucastrate;
	else {
		(void) ieee80211_amrr_choose(ni, &wn->amn);
		rate = ni->ni_txrate;
	}
	ridx = iwn_plcp_signal(rate);
	rinfo = &iwn_rates[ridx];

	/* Encrypt the frame if need be. */
	if (wh->i_fc[1] & IEEE80211_FC1_WEP) {
		k = ieee80211_crypto_encap(ni, m);
		if (k == NULL) {
			m_freem(m);
			return ENOBUFS;
		}
		/* Packet header may have moved, reset our local pointer. */
		wh = mtod(m, struct ieee80211_frame *);
	}
	totlen = m->m_pkthdr.len;

	if (ieee80211_radiotap_active_vap(vap)) {
		struct iwn_tx_radiotap_header *tap = &sc->sc_txtap;

		tap->wt_flags = 0;
		tap->wt_rate = rinfo->rate;
		if (k != NULL)
			tap->wt_flags |= IEEE80211_RADIOTAP_F_WEP;

		ieee80211_radiotap_tx(vap, m);
	}

	/* Prepare TX firmware command. */
	cmd = &ring->cmd[ring->cur];
	cmd->code = IWN_CMD_TX_DATA;
	cmd->flags = 0;
	cmd->qid = ring->qid;
	cmd->idx = ring->cur;

	tx = (struct iwn_cmd_data *)cmd->data;
	/* NB: No need to clear tx, all fields are reinitialized here. */
	tx->scratch = 0;	/* clear "scratch" area */

	flags = 0;
	if (!IEEE80211_IS_MULTICAST(wh->i_addr1))
		flags |= IWN_TX_NEED_ACK;
	if ((wh->i_fc[0] &
	    (IEEE80211_FC0_TYPE_MASK | IEEE80211_FC0_SUBTYPE_MASK)) ==
	    (IEEE80211_FC0_TYPE_CTL | IEEE80211_FC0_SUBTYPE_BAR))
		flags |= IWN_TX_IMM_BA;		/* Cannot happen yet. */

	if (wh->i_fc[1] & IEEE80211_FC1_MORE_FRAG)
		flags |= IWN_TX_MORE_FRAG;	/* Cannot happen yet. */

	/* Check if frame must be protected using RTS/CTS or CTS-to-self. */
	if (!IEEE80211_IS_MULTICAST(wh->i_addr1)) {
		/* NB: Group frames are sent using CCK in 802.11b/g. */
		if (totlen + IEEE80211_CRC_LEN > vap->iv_rtsthreshold) {
			flags |= IWN_TX_NEED_RTS;
		} else if ((ic->ic_flags & IEEE80211_F_USEPROT) &&
		    ridx >= IWN_RIDX_OFDM6) {
			if (ic->ic_protmode == IEEE80211_PROT_CTSONLY)
				flags |= IWN_TX_NEED_CTS;
			else if (ic->ic_protmode == IEEE80211_PROT_RTSCTS)
				flags |= IWN_TX_NEED_RTS;
		}
		if (flags & (IWN_TX_NEED_RTS | IWN_TX_NEED_CTS)) {
			if (sc->hw_type != IWN_HW_REV_TYPE_4965) {
				/* 5000 autoselects RTS/CTS or CTS-to-self. */
				flags &= ~(IWN_TX_NEED_RTS | IWN_TX_NEED_CTS);
				flags |= IWN_TX_NEED_PROTECTION;
			} else
				flags |= IWN_TX_FULL_TXOP;
		}
	}

	if (IEEE80211_IS_MULTICAST(wh->i_addr1) ||
	    type != IEEE80211_FC0_TYPE_DATA)
		tx->id = hal->broadcast_id;
	else
		tx->id = wn->id;

	if (type == IEEE80211_FC0_TYPE_MGT) {
		uint8_t subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;

		/* Tell HW to set timestamp in probe responses. */
		if (subtype == IEEE80211_FC0_SUBTYPE_PROBE_RESP)
			flags |= IWN_TX_INSERT_TSTAMP;

		if (subtype == IEEE80211_FC0_SUBTYPE_ASSOC_REQ ||
		    subtype == IEEE80211_FC0_SUBTYPE_REASSOC_REQ)
			tx->timeout = htole16(3);
		else
			tx->timeout = htole16(2);
	} else
		tx->timeout = htole16(0);

	if (hdrlen & 3) {
		/* First segment length must be a multiple of 4. */
		flags |= IWN_TX_NEED_PADDING;
		pad = 4 - (hdrlen & 3);
	} else
		pad = 0;

	tx->len = htole16(totlen);
	tx->tid = 0;
	tx->rts_ntries = 60;
	tx->data_ntries = 15;
	tx->lifetime = htole32(IWN_LIFETIME_INFINITE);
	tx->plcp = rinfo->plcp;
	tx->rflags = rinfo->flags;
	if (tx->id == hal->broadcast_id) {
		/* Group or management frame. */
		tx->linkq = 0;
		/* XXX Alternate between antenna A and B? */
		txant = IWN_LSB(sc->txchainmask);
		tx->rflags |= IWN_RFLAG_ANT(txant);
	} else {
		tx->linkq = 0;
		flags |= IWN_TX_LINKQ;	/* enable MRR */
	}

	/* Set physical address of "scratch area". */
	tx->loaddr = htole32(IWN_LOADDR(data->scratch_paddr));
	tx->hiaddr = IWN_HIADDR(data->scratch_paddr);

	/* Copy 802.11 header in TX command. */
	memcpy((uint8_t *)(tx + 1), wh, hdrlen);

	/* Trim 802.11 header. */
	m_adj(m, hdrlen);
	tx->security = 0;
	tx->flags = htole32(flags);

	if (m->m_len > 0) {
		error = bus_dmamap_load_mbuf_sg(ring->data_dmat, data->map,
		    m, segs, &nsegs, BUS_DMA_NOWAIT);
		if (error == EFBIG) {
			/* too many fragments, linearize */
			mnew = m_collapse(m, M_DONTWAIT, IWN_MAX_SCATTER);
			if (mnew == NULL) {
				device_printf(sc->sc_dev,
				    "%s: could not defrag mbuf\n", __func__);
				m_freem(m);
				return ENOBUFS;
			}
			m = mnew;
			error = bus_dmamap_load_mbuf_sg(ring->data_dmat,
			    data->map, m, segs, &nsegs, BUS_DMA_NOWAIT);
		}
		if (error != 0) {
			device_printf(sc->sc_dev,
			    "%s: bus_dmamap_load_mbuf_sg failed, error %d\n",
			    __func__, error);
			m_freem(m);
			return error;
		}
	}

	data->m = m;
	data->ni = ni;

	DPRINTF(sc, IWN_DEBUG_XMIT, "%s: qid %d idx %d len %d nsegs %d\n",
	    __func__, ring->qid, ring->cur, m->m_pkthdr.len, nsegs);

	/* Fill TX descriptor. */
	desc->nsegs = 1 + nsegs;
	/* First DMA segment is used by the TX command. */
	desc->segs[0].addr = htole32(IWN_LOADDR(data->cmd_paddr));
	desc->segs[0].len  = htole16(IWN_HIADDR(data->cmd_paddr) |
	    (4 + sizeof (*tx) + hdrlen + pad) << 4);
	/* Other DMA segments are for data payload. */
	for (i = 1; i <= nsegs; i++) {
		desc->segs[i].addr = htole32(IWN_LOADDR(segs[i - 1].ds_addr));
		desc->segs[i].len  = htole16(IWN_HIADDR(segs[i - 1].ds_addr) |
		    segs[i - 1].ds_len << 4);
	}

	bus_dmamap_sync(ring->data_dmat, data->map, BUS_DMASYNC_PREWRITE);
	bus_dmamap_sync(ring->data_dmat, ring->cmd_dma.map,
	    BUS_DMASYNC_PREWRITE);
	bus_dmamap_sync(ring->desc_dma.tag, ring->desc_dma.map,
	    BUS_DMASYNC_PREWRITE);

#ifdef notyet
	/* Update TX scheduler. */
	hal->update_sched(sc, ring->qid, ring->cur, tx->id, totlen);
#endif

	/* Kick TX ring. */
	ring->cur = (ring->cur + 1) % IWN_TX_RING_COUNT;
	IWN_WRITE(sc, IWN_HBUS_TARG_WRPTR, ring->qid << 8 | ring->cur);

	/* Mark TX ring as full if we reach a certain threshold. */
	if (++ring->queued > IWN_TX_RING_HIMARK)
		sc->qfullmsk |= 1 << ring->qid;

	return 0;
}

static int
iwn_tx_data_raw(struct iwn_softc *sc, struct mbuf *m,
    struct ieee80211_node *ni, struct iwn_tx_ring *ring,
    const struct ieee80211_bpf_params *params)
{
	const struct iwn_hal *hal = sc->sc_hal;
	const struct iwn_rate *rinfo;
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211vap *vap = ni->ni_vap;
	struct ieee80211com *ic = ifp->if_l2com;
	struct iwn_tx_cmd *cmd;
	struct iwn_cmd_data *tx;
	struct ieee80211_frame *wh;
	struct iwn_tx_desc *desc;
	struct iwn_tx_data *data;
	struct mbuf *mnew;
	bus_addr_t paddr;
	bus_dma_segment_t segs[IWN_MAX_SCATTER];
	uint32_t flags;
	u_int hdrlen;
	int totlen, error, pad, nsegs = 0, i, rate;
	uint8_t ridx, type, txant;

	IWN_LOCK_ASSERT(sc);

	wh = mtod(m, struct ieee80211_frame *);
	hdrlen = ieee80211_anyhdrsize(wh);
	type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;

	desc = &ring->desc[ring->cur];
	data = &ring->data[ring->cur];

	/* Choose a TX rate index. */
	rate = params->ibp_rate0;
	if (!ieee80211_isratevalid(ic->ic_rt, rate)) {
		/* XXX fall back to mcast/mgmt rate? */
		m_freem(m);
		return EINVAL;
	}
	ridx = iwn_plcp_signal(rate);
	rinfo = &iwn_rates[ridx];

	totlen = m->m_pkthdr.len;

	/* Prepare TX firmware command. */
	cmd = &ring->cmd[ring->cur];
	cmd->code = IWN_CMD_TX_DATA;
	cmd->flags = 0;
	cmd->qid = ring->qid;
	cmd->idx = ring->cur;

	tx = (struct iwn_cmd_data *)cmd->data;
	/* NB: No need to clear tx, all fields are reinitialized here. */
	tx->scratch = 0;	/* clear "scratch" area */

	flags = 0;
	if ((params->ibp_flags & IEEE80211_BPF_NOACK) == 0)
		flags |= IWN_TX_NEED_ACK;
	if (params->ibp_flags & IEEE80211_BPF_RTS) {
		if (sc->hw_type != IWN_HW_REV_TYPE_4965) {
			/* 5000 autoselects RTS/CTS or CTS-to-self. */
			flags &= ~IWN_TX_NEED_RTS;
			flags |= IWN_TX_NEED_PROTECTION;
		} else
			flags |= IWN_TX_NEED_RTS | IWN_TX_FULL_TXOP;
	}
	if (params->ibp_flags & IEEE80211_BPF_CTS) {
		if (sc->hw_type != IWN_HW_REV_TYPE_4965) {
			/* 5000 autoselects RTS/CTS or CTS-to-self. */
			flags &= ~IWN_TX_NEED_CTS;
			flags |= IWN_TX_NEED_PROTECTION;
		} else
			flags |= IWN_TX_NEED_CTS | IWN_TX_FULL_TXOP;
	}
	if (type == IEEE80211_FC0_TYPE_MGT) {
		uint8_t subtype = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;

		if (subtype == IEEE80211_FC0_SUBTYPE_PROBE_RESP)
			flags |= IWN_TX_INSERT_TSTAMP;

		if (subtype == IEEE80211_FC0_SUBTYPE_ASSOC_REQ ||
		    subtype == IEEE80211_FC0_SUBTYPE_REASSOC_REQ)
			tx->timeout = htole16(3);
		else
			tx->timeout = htole16(2);
	} else
		tx->timeout = htole16(0);

	if (hdrlen & 3) {
		/* First segment length must be a multiple of 4. */
		flags |= IWN_TX_NEED_PADDING;
		pad = 4 - (hdrlen & 3);
	} else
		pad = 0;

	if (ieee80211_radiotap_active_vap(vap)) {
		struct iwn_tx_radiotap_header *tap = &sc->sc_txtap;

		tap->wt_flags = 0;
		tap->wt_rate = rate;

		ieee80211_radiotap_tx(vap, m);
	}

	tx->len = htole16(totlen);
	tx->tid = 0;
	tx->id = hal->broadcast_id;
	tx->rts_ntries = params->ibp_try1;
	tx->data_ntries = params->ibp_try0;
	tx->lifetime = htole32(IWN_LIFETIME_INFINITE);
	tx->plcp = rinfo->plcp;
	tx->rflags = rinfo->flags;
	/* Group or management frame. */
	tx->linkq = 0;
	txant = IWN_LSB(sc->txchainmask);
	tx->rflags |= IWN_RFLAG_ANT(txant);
	/* Set physical address of "scratch area". */
	paddr = ring->cmd_dma.paddr + ring->cur * sizeof (struct iwn_tx_cmd);
	tx->loaddr = htole32(IWN_LOADDR(paddr));
	tx->hiaddr = IWN_HIADDR(paddr);

	/* Copy 802.11 header in TX command. */
	memcpy((uint8_t *)(tx + 1), wh, hdrlen);

	/* Trim 802.11 header. */
	m_adj(m, hdrlen);
	tx->security = 0;
	tx->flags = htole32(flags);

	if (m->m_len > 0) {
		error = bus_dmamap_load_mbuf_sg(ring->data_dmat, data->map,
		    m, segs, &nsegs, BUS_DMA_NOWAIT);
		if (error == EFBIG) {
			/* Too many fragments, linearize. */
			mnew = m_collapse(m, M_DONTWAIT, IWN_MAX_SCATTER);
			if (mnew == NULL) {
				device_printf(sc->sc_dev,
				    "%s: could not defrag mbuf\n", __func__);
				m_freem(m);
				return ENOBUFS;
			}
			m = mnew;
			error = bus_dmamap_load_mbuf_sg(ring->data_dmat,
			    data->map, m, segs, &nsegs, BUS_DMA_NOWAIT);
		}
		if (error != 0) {
			device_printf(sc->sc_dev,
			    "%s: bus_dmamap_load_mbuf_sg failed, error %d\n",
			    __func__, error);
			m_freem(m);
			return error;
		}
	}

	data->m = m;
	data->ni = ni;

	DPRINTF(sc, IWN_DEBUG_XMIT, "%s: qid %d idx %d len %d nsegs %d\n",
	    __func__, ring->qid, ring->cur, m->m_pkthdr.len, nsegs);

	/* Fill TX descriptor. */
	desc->nsegs = 1 + nsegs;
	/* First DMA segment is used by the TX command. */
	desc->segs[0].addr = htole32(IWN_LOADDR(data->cmd_paddr));
	desc->segs[0].len  = htole16(IWN_HIADDR(data->cmd_paddr) |
	    (4 + sizeof (*tx) + hdrlen + pad) << 4);
	/* Other DMA segments are for data payload. */
	for (i = 1; i <= nsegs; i++) {
		desc->segs[i].addr = htole32(IWN_LOADDR(segs[i - 1].ds_addr));
		desc->segs[i].len  = htole16(IWN_HIADDR(segs[i - 1].ds_addr) |
		    segs[i - 1].ds_len << 4);
	}

	bus_dmamap_sync(ring->data_dmat, data->map, BUS_DMASYNC_PREWRITE);
	bus_dmamap_sync(ring->data_dmat, ring->cmd_dma.map,
	    BUS_DMASYNC_PREWRITE);
	bus_dmamap_sync(ring->desc_dma.tag, ring->desc_dma.map,
	    BUS_DMASYNC_PREWRITE);

#ifdef notyet
	/* Update TX scheduler. */
	hal->update_sched(sc, ring->qid, ring->cur, tx->id, totlen);
#endif

	/* Kick TX ring. */
	ring->cur = (ring->cur + 1) % IWN_TX_RING_COUNT;
	IWN_WRITE(sc, IWN_HBUS_TARG_WRPTR, ring->qid << 8 | ring->cur);

	/* Mark TX ring as full if we reach a certain threshold. */
	if (++ring->queued > IWN_TX_RING_HIMARK)
		sc->qfullmsk |= 1 << ring->qid;

	return 0;
}

static int
iwn_raw_xmit(struct ieee80211_node *ni, struct mbuf *m,
	const struct ieee80211_bpf_params *params)
{
	struct ieee80211com *ic = ni->ni_ic;
	struct ifnet *ifp = ic->ic_ifp;
	struct iwn_softc *sc = ifp->if_softc;
	struct iwn_tx_ring *txq;
	int error = 0;

	if ((ifp->if_drv_flags & IFF_DRV_RUNNING) == 0) {
		ieee80211_free_node(ni);
		m_freem(m);
		return ENETDOWN;
	}

	IWN_LOCK(sc);
	if (params == NULL)
		txq = &sc->txq[M_WME_GETAC(m)];
	else
		txq = &sc->txq[params->ibp_pri & 3];

	if (params == NULL) {
		/*
		 * Legacy path; interpret frame contents to decide
		 * precisely how to send the frame.
		 */
		error = iwn_tx_data(sc, m, ni, txq);
	} else {
		/*
		 * Caller supplied explicit parameters to use in
		 * sending the frame.
		 */
		error = iwn_tx_data_raw(sc, m, ni, txq, params);
	}
	if (error != 0) {
		/* NB: m is reclaimed on tx failure */
		ieee80211_free_node(ni);
		ifp->if_oerrors++;
	}
	IWN_UNLOCK(sc);
	return error;
}

static void
iwn_start(struct ifnet *ifp)
{
	struct iwn_softc *sc = ifp->if_softc;

	IWN_LOCK(sc);
	iwn_start_locked(ifp);
	IWN_UNLOCK(sc);
}

static void
iwn_start_locked(struct ifnet *ifp)
{
	struct iwn_softc *sc = ifp->if_softc;
	struct ieee80211_node *ni;
	struct iwn_tx_ring *txq;
	struct mbuf *m;
	int pri;

	IWN_LOCK_ASSERT(sc);

	for (;;) {
		if (sc->qfullmsk != 0) {
			ifp->if_drv_flags |= IFF_DRV_OACTIVE;
			break;
		}
		IFQ_DRV_DEQUEUE(&ifp->if_snd, m);
		if (m == NULL)
			break;
		ni = (struct ieee80211_node *)m->m_pkthdr.rcvif;
		pri = M_WME_GETAC(m);
		txq = &sc->txq[pri];
		if (iwn_tx_data(sc, m, ni, txq) != 0) {
			ifp->if_oerrors++;
			ieee80211_free_node(ni);
			break;
		}
		sc->sc_tx_timer = 5;
	}
}

static void
iwn_watchdog(struct iwn_softc *sc)
{
	if (sc->sc_tx_timer > 0 && --sc->sc_tx_timer == 0) {
		struct ifnet *ifp = sc->sc_ifp;
		struct ieee80211com *ic = ifp->if_l2com;

		if_printf(ifp, "device timeout\n");
		ieee80211_runtask(ic, &sc->sc_reinit_task);
	}
}

static int
iwn_ioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct iwn_softc *sc = ifp->if_softc;
	struct ieee80211com *ic = ifp->if_l2com;
	struct ieee80211vap *vap = TAILQ_FIRST(&ic->ic_vaps);
	struct ifreq *ifr = (struct ifreq *) data;
	int error = 0, startall = 0, stop = 0;

	switch (cmd) {
	case SIOCSIFFLAGS:
		IWN_LOCK(sc);
		if (ifp->if_flags & IFF_UP) {
			if (!(ifp->if_drv_flags & IFF_DRV_RUNNING)) {
				iwn_init_locked(sc);
				if (IWN_READ(sc, IWN_GP_CNTRL) & IWN_GP_CNTRL_RFKILL)
					startall = 1;
				else
					stop = 1;
			}
		} else {
			if (ifp->if_drv_flags & IFF_DRV_RUNNING)
				iwn_stop_locked(sc);
		}
		IWN_UNLOCK(sc);
		if (startall)
			ieee80211_start_all(ic);
		else if (vap != NULL && stop)
			ieee80211_stop(vap);
		break;
	case SIOCGIFMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &ic->ic_media, cmd);
		break;
	case SIOCGIFADDR:
		error = ether_ioctl(ifp, cmd, data);
		break;
	default:
		error = EINVAL;
		break;
	}
	return error;
}

/*
 * Send a command to the firmware.
 */
static int
iwn_cmd(struct iwn_softc *sc, int code, const void *buf, int size, int async)
{
	struct iwn_tx_ring *ring = &sc->txq[4];
	struct iwn_tx_desc *desc;
	struct iwn_tx_data *data;
	struct iwn_tx_cmd *cmd;
	struct mbuf *m;
	bus_addr_t paddr;
	int totlen, error;

	IWN_LOCK_ASSERT(sc);

	desc = &ring->desc[ring->cur];
	data = &ring->data[ring->cur];
	totlen = 4 + size;

	if (size > sizeof cmd->data) {
		/* Command is too large to fit in a descriptor. */
		if (totlen > MCLBYTES)
			return EINVAL;
		m = m_getjcl(M_DONTWAIT, MT_DATA, M_PKTHDR, MJUMPAGESIZE);
		if (m == NULL)
			return ENOMEM;
		cmd = mtod(m, struct iwn_tx_cmd *);
		error = bus_dmamap_load(ring->data_dmat, data->map, cmd,
		    totlen, iwn_dma_map_addr, &paddr, BUS_DMA_NOWAIT);
		if (error != 0) {
			m_freem(m);
			return error;
		}
		data->m = m;
	} else {
		cmd = &ring->cmd[ring->cur];
		paddr = data->cmd_paddr;
	}

	cmd->code = code;
	cmd->flags = 0;
	cmd->qid = ring->qid;
	cmd->idx = ring->cur;
	memcpy(cmd->data, buf, size);

	desc->nsegs = 1;
	desc->segs[0].addr = htole32(IWN_LOADDR(paddr));
	desc->segs[0].len  = htole16(IWN_HIADDR(paddr) | totlen << 4);

	DPRINTF(sc, IWN_DEBUG_CMD, "%s: %s (0x%x) flags %d qid %d idx %d\n",
	    __func__, iwn_intr_str(cmd->code), cmd->code,
	    cmd->flags, cmd->qid, cmd->idx);

	if (size > sizeof cmd->data) {
		bus_dmamap_sync(ring->data_dmat, data->map,
		    BUS_DMASYNC_PREWRITE);
	} else {
		bus_dmamap_sync(ring->data_dmat, ring->cmd_dma.map,
		    BUS_DMASYNC_PREWRITE);
	}
	bus_dmamap_sync(ring->desc_dma.tag, ring->desc_dma.map,
	    BUS_DMASYNC_PREWRITE);

#ifdef notyet
	/* Update TX scheduler. */
	sc->sc_hal->update_sched(sc, ring->qid, ring->cur, 0, 0);
#endif

	/* Kick command ring. */
	ring->cur = (ring->cur + 1) % IWN_TX_RING_COUNT;
	IWN_WRITE(sc, IWN_HBUS_TARG_WRPTR, ring->qid << 8 | ring->cur);

	return async ? 0 : msleep(desc, &sc->sc_mtx, PCATCH, "iwncmd", hz);
}

static int
iwn4965_add_node(struct iwn_softc *sc, struct iwn_node_info *node, int async)
{
	struct iwn4965_node_info hnode;
	caddr_t src, dst;

	/*
	 * We use the node structure for 5000 Series internally (it is
	 * a superset of the one for 4965AGN). We thus copy the common
	 * fields before sending the command.
	 */
	src = (caddr_t)node;
	dst = (caddr_t)&hnode;
	memcpy(dst, src, 48);
	/* Skip TSC, RX MIC and TX MIC fields from ``src''. */
	memcpy(dst + 48, src + 72, 20);
	return iwn_cmd(sc, IWN_CMD_ADD_NODE, &hnode, sizeof hnode, async);
}

static int
iwn5000_add_node(struct iwn_softc *sc, struct iwn_node_info *node, int async)
{
	/* Direct mapping. */
	return iwn_cmd(sc, IWN_CMD_ADD_NODE, node, sizeof (*node), async);
}

#if 0	/* HT */
static const uint8_t iwn_ridx_to_plcp[] = {
	10, 20, 55, 110, /* CCK */
	0xd, 0xf, 0x5, 0x7, 0x9, 0xb, 0x1, 0x3, 0x3 /* OFDM R1-R4 */
};
static const uint8_t iwn_siso_mcs_to_plcp[] = {
	0, 0, 0, 0, 			/* CCK */
	0, 0, 1, 2, 3, 4, 5, 6, 7	/* HT */
};
static const uint8_t iwn_mimo_mcs_to_plcp[] = {
	0, 0, 0, 0, 			/* CCK */
	8, 8, 9, 10, 11, 12, 13, 14, 15	/* HT */
};
#endif
static const uint8_t iwn_prev_ridx[] = {
	/* NB: allow fallback from CCK11 to OFDM9 and from OFDM6 to CCK5 */
	0, 0, 1, 5,			/* CCK */
	2, 4, 3, 6, 7, 8, 9, 10, 10	/* OFDM */
};

/*
 * Configure hardware link parameters for the specified
 * node operating on the specified channel.
 */
static int
iwn_set_link_quality(struct iwn_softc *sc, uint8_t id, int async)
{
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	struct iwn_cmd_link_quality linkq;
	const struct iwn_rate *rinfo;
	int i;
	uint8_t txant, ridx;

	/* Use the first valid TX antenna. */
	txant = IWN_LSB(sc->txchainmask);

	memset(&linkq, 0, sizeof linkq);
	linkq.id = id;
	linkq.antmsk_1stream = txant;
	linkq.antmsk_2stream = IWN_ANT_AB;
	linkq.ampdu_max = 31;
	linkq.ampdu_threshold = 3;
	linkq.ampdu_limit = htole16(4000);	/* 4ms */

#if 0	/* HT */
	if (IEEE80211_IS_CHAN_HT(c))
		linkq.mimo = 1;
#endif

	if (id == IWN_ID_BSS)
		ridx = IWN_RIDX_OFDM54;
	else if (IEEE80211_IS_CHAN_A(ic->ic_curchan))
		ridx = IWN_RIDX_OFDM6;
	else
		ridx = IWN_RIDX_CCK1;

	for (i = 0; i < IWN_MAX_TX_RETRIES; i++) {
		rinfo = &iwn_rates[ridx];
#if 0	/* HT */
		if (IEEE80211_IS_CHAN_HT40(c)) {
			linkq.retry[i].plcp = iwn_mimo_mcs_to_plcp[ridx]
					 | IWN_RIDX_MCS;
			linkq.retry[i].rflags = IWN_RFLAG_HT
					 | IWN_RFLAG_HT40;
			/* XXX shortGI */
		} else if (IEEE80211_IS_CHAN_HT(c)) {
			linkq.retry[i].plcp = iwn_siso_mcs_to_plcp[ridx]
					 | IWN_RIDX_MCS;
			linkq.retry[i].rflags = IWN_RFLAG_HT;
			/* XXX shortGI */
		} else
#endif
		{
			linkq.retry[i].plcp = rinfo->plcp;
			linkq.retry[i].rflags = rinfo->flags;
		}
		linkq.retry[i].rflags |= IWN_RFLAG_ANT(txant);
		ridx = iwn_prev_ridx[ridx];
	}
#ifdef IWN_DEBUG
	if (sc->sc_debug & IWN_DEBUG_STATE) {
		printf("%s: set link quality for node %d, mimo %d ssmask %d\n",
		    __func__, id, linkq.mimo, linkq.antmsk_1stream);
		printf("%s:", __func__);
		for (i = 0; i < IWN_MAX_TX_RETRIES; i++)
			printf(" %d:%x", linkq.retry[i].plcp,
			    linkq.retry[i].rflags);
		printf("\n");
	}
#endif
	return iwn_cmd(sc, IWN_CMD_LINK_QUALITY, &linkq, sizeof linkq, async);
}

/*
 * Broadcast node is used to send group-addressed and management frames.
 */
static int
iwn_add_broadcast_node(struct iwn_softc *sc, int async)
{
	const struct iwn_hal *hal = sc->sc_hal;
	struct ifnet *ifp = sc->sc_ifp;
	struct iwn_node_info node;
	int error;

	memset(&node, 0, sizeof node);
	IEEE80211_ADDR_COPY(node.macaddr, ifp->if_broadcastaddr);
	node.id = hal->broadcast_id;
	DPRINTF(sc, IWN_DEBUG_RESET, "%s: adding broadcast node\n", __func__);
	error = hal->add_node(sc, &node, async);
	if (error != 0)
		return error;

	error = iwn_set_link_quality(sc, hal->broadcast_id, async);
	return error;
}

static int
iwn_wme_update(struct ieee80211com *ic)
{
#define IWN_EXP2(x)	((1 << (x)) - 1)	/* CWmin = 2^ECWmin - 1 */
#define	IWN_TXOP_TO_US(v)		(v<<5)
	struct iwn_softc *sc = ic->ic_ifp->if_softc;
	struct iwn_edca_params cmd;
	int i;

	memset(&cmd, 0, sizeof cmd);
	cmd.flags = htole32(IWN_EDCA_UPDATE);
	for (i = 0; i < WME_NUM_AC; i++) {
		const struct wmeParams *wmep =
		    &ic->ic_wme.wme_chanParams.cap_wmeParams[i];
		cmd.ac[i].aifsn = wmep->wmep_aifsn;
		cmd.ac[i].cwmin = htole16(IWN_EXP2(wmep->wmep_logcwmin));
		cmd.ac[i].cwmax = htole16(IWN_EXP2(wmep->wmep_logcwmax));
		cmd.ac[i].txoplimit =
		    htole16(IWN_TXOP_TO_US(wmep->wmep_txopLimit));
	}
	IEEE80211_UNLOCK(ic);
	IWN_LOCK(sc);
	(void) iwn_cmd(sc, IWN_CMD_EDCA_PARAMS, &cmd, sizeof cmd, 1 /*async*/);
	IWN_UNLOCK(sc);
	IEEE80211_LOCK(ic);
	return 0;
#undef IWN_TXOP_TO_US
#undef IWN_EXP2
}

static void
iwn_update_mcast(struct ifnet *ifp)
{
	/* Ignore */
}

static void
iwn_set_led(struct iwn_softc *sc, uint8_t which, uint8_t off, uint8_t on)
{
	struct iwn_cmd_led led;

	/* Clear microcode LED ownership. */
	IWN_CLRBITS(sc, IWN_LED, IWN_LED_BSM_CTRL);

	led.which = which;
	led.unit = htole32(10000);	/* on/off in unit of 100ms */
	led.off = off;
	led.on = on;
	(void)iwn_cmd(sc, IWN_CMD_SET_LED, &led, sizeof led, 1);
}

/*
 * Set the critical temperature at which the firmware will stop the radio
 * and notify us.
 */
static int
iwn_set_critical_temp(struct iwn_softc *sc)
{
	struct iwn_critical_temp crit;
	int32_t temp;

	IWN_WRITE(sc, IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_CTEMP_STOP_RF);

	if (sc->hw_type == IWN_HW_REV_TYPE_5150)
		temp = (IWN_CTOK(110) - sc->temp_off) * -5;
	else if (sc->hw_type == IWN_HW_REV_TYPE_4965)
		temp = IWN_CTOK(110);
	else
		temp = 110;
	memset(&crit, 0, sizeof crit);
	crit.tempR = htole32(temp);
	DPRINTF(sc, IWN_DEBUG_RESET, "setting critical temp to %d\n",
	    temp);
	return iwn_cmd(sc, IWN_CMD_SET_CRITICAL_TEMP, &crit, sizeof crit, 0);
}

static int
iwn_set_timing(struct iwn_softc *sc, struct ieee80211_node *ni)
{
	struct iwn_cmd_timing cmd;
	uint64_t val, mod;

	memset(&cmd, 0, sizeof cmd);
	memcpy(&cmd.tstamp, ni->ni_tstamp.data, sizeof (uint64_t));
	cmd.bintval = htole16(ni->ni_intval);
	cmd.lintval = htole16(10);

	/* Compute remaining time until next beacon. */
	val = (uint64_t)ni->ni_intval * 1024;	/* msecs -> usecs */
	mod = le64toh(cmd.tstamp) % val;
	cmd.binitval = htole32((uint32_t)(val - mod));

	DPRINTF(sc, IWN_DEBUG_RESET, "timing bintval=%u tstamp=%ju, init=%u\n",
	    ni->ni_intval, le64toh(cmd.tstamp), (uint32_t)(val - mod));

	return iwn_cmd(sc, IWN_CMD_TIMING, &cmd, sizeof cmd, 1);
}

static void
iwn4965_power_calibration(struct iwn_softc *sc, int temp)
{
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;

	/* Adjust TX power if need be (delta >= 3 degC.) */
	DPRINTF(sc, IWN_DEBUG_CALIBRATE, "%s: temperature %d->%d\n",
	    __func__, sc->temp, temp);
	if (abs(temp - sc->temp) >= 3) {
		/* Record temperature of last calibration. */
		sc->temp = temp;
		(void)iwn4965_set_txpower(sc, ic->ic_bsschan, 1);
	}
}

/*
 * Set TX power for current channel (each rate has its own power settings).
 * This function takes into account the regulatory information from EEPROM,
 * the current temperature and the current voltage.
 */
static int
iwn4965_set_txpower(struct iwn_softc *sc, struct ieee80211_channel *ch,
    int async)
{
/* Fixed-point arithmetic division using a n-bit fractional part. */
#define fdivround(a, b, n)	\
	((((1 << n) * (a)) / (b) + (1 << n) / 2) / (1 << n))
/* Linear interpolation. */
#define interpolate(x, x1, y1, x2, y2, n)	\
	((y1) + fdivround(((int)(x) - (x1)) * ((y2) - (y1)), (x2) - (x1), n))

	static const int tdiv[IWN_NATTEN_GROUPS] = { 9, 8, 8, 8, 6 };
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	struct iwn_ucode_info *uc = &sc->ucode_info;
	struct iwn4965_cmd_txpower cmd;
	struct iwn4965_eeprom_chan_samples *chans;
	int32_t vdiff, tdiff;
	int i, c, grp, maxpwr;
	const uint8_t *rf_gain, *dsp_gain;
	uint8_t chan;

	/* Retrieve channel number. */
	chan = ieee80211_chan2ieee(ic, ch);
	DPRINTF(sc, IWN_DEBUG_RESET, "setting TX power for channel %d\n",
	    chan);

	memset(&cmd, 0, sizeof cmd);
	cmd.band = IEEE80211_IS_CHAN_5GHZ(ch) ? 0 : 1;
	cmd.chan = chan;

	if (IEEE80211_IS_CHAN_5GHZ(ch)) {
		maxpwr   = sc->maxpwr5GHz;
		rf_gain  = iwn4965_rf_gain_5ghz;
		dsp_gain = iwn4965_dsp_gain_5ghz;
	} else {
		maxpwr   = sc->maxpwr2GHz;
		rf_gain  = iwn4965_rf_gain_2ghz;
		dsp_gain = iwn4965_dsp_gain_2ghz;
	}

	/* Compute voltage compensation. */
	vdiff = ((int32_t)le32toh(uc->volt) - sc->eeprom_voltage) / 7;
	if (vdiff > 0)
		vdiff *= 2;
	if (abs(vdiff) > 2)
		vdiff = 0;
	DPRINTF(sc, IWN_DEBUG_CALIBRATE | IWN_DEBUG_TXPOW,
	    "%s: voltage compensation=%d (UCODE=%d, EEPROM=%d)\n",
	    __func__, vdiff, le32toh(uc->volt), sc->eeprom_voltage);

	/* Get channel attenuation group. */
	if (chan <= 20)		/* 1-20 */
		grp = 4;
	else if (chan <= 43)	/* 34-43 */
		grp = 0;
	else if (chan <= 70)	/* 44-70 */
		grp = 1;
	else if (chan <= 124)	/* 71-124 */
		grp = 2;
	else			/* 125-200 */
		grp = 3;
	DPRINTF(sc, IWN_DEBUG_CALIBRATE | IWN_DEBUG_TXPOW,
	    "%s: chan %d, attenuation group=%d\n", __func__, chan, grp);

	/* Get channel sub-band. */
	for (i = 0; i < IWN_NBANDS; i++)
		if (sc->bands[i].lo != 0 &&
		    sc->bands[i].lo <= chan && chan <= sc->bands[i].hi)
			break;
	if (i == IWN_NBANDS)	/* Can't happen in real-life. */
		return EINVAL;
	chans = sc->bands[i].chans;
	DPRINTF(sc, IWN_DEBUG_CALIBRATE | IWN_DEBUG_TXPOW,
	    "%s: chan %d sub-band=%d\n", __func__, chan, i);

	for (c = 0; c < 2; c++) {
		uint8_t power, gain, temp;
		int maxchpwr, pwr, ridx, idx;

		power = interpolate(chan,
		    chans[0].num, chans[0].samples[c][1].power,
		    chans[1].num, chans[1].samples[c][1].power, 1);
		gain  = interpolate(chan,
		    chans[0].num, chans[0].samples[c][1].gain,
		    chans[1].num, chans[1].samples[c][1].gain, 1);
		temp  = interpolate(chan,
		    chans[0].num, chans[0].samples[c][1].temp,
		    chans[1].num, chans[1].samples[c][1].temp, 1);
		DPRINTF(sc, IWN_DEBUG_CALIBRATE | IWN_DEBUG_TXPOW,
		    "%s: Tx chain %d: power=%d gain=%d temp=%d\n",
		    __func__, c, power, gain, temp);

		/* Compute temperature compensation. */
		tdiff = ((sc->temp - temp) * 2) / tdiv[grp];
		DPRINTF(sc, IWN_DEBUG_CALIBRATE | IWN_DEBUG_TXPOW,
		    "%s: temperature compensation=%d (current=%d, EEPROM=%d)\n",
		    __func__, tdiff, sc->temp, temp);

		for (ridx = 0; ridx <= IWN_RIDX_MAX; ridx++) {
			/* Convert dBm to half-dBm. */
			maxchpwr = sc->maxpwr[chan] * 2;
			if ((ridx / 8) & 1)
				maxchpwr -= 6;	/* MIMO 2T: -3dB */

			pwr = maxpwr;

			/* Adjust TX power based on rate. */
			if ((ridx % 8) == 5)
				pwr -= 15;	/* OFDM48: -7.5dB */
			else if ((ridx % 8) == 6)
				pwr -= 17;	/* OFDM54: -8.5dB */
			else if ((ridx % 8) == 7)
				pwr -= 20;	/* OFDM60: -10dB */
			else
				pwr -= 10;	/* Others: -5dB */

			/* Do not exceed channel max TX power. */
			if (pwr > maxchpwr)
				pwr = maxchpwr;

			idx = gain - (pwr - power) - tdiff - vdiff;
			if ((ridx / 8) & 1)	/* MIMO */
				idx += (int32_t)le32toh(uc->atten[grp][c]);

			if (cmd.band == 0)
				idx += 9;	/* 5GHz */
			if (ridx == IWN_RIDX_MAX)
				idx += 5;	/* CCK */

			/* Make sure idx stays in a valid range. */
			if (idx < 0)
				idx = 0;
			else if (idx > IWN4965_MAX_PWR_INDEX)
				idx = IWN4965_MAX_PWR_INDEX;

			DPRINTF(sc, IWN_DEBUG_CALIBRATE | IWN_DEBUG_TXPOW,
			    "%s: Tx chain %d, rate idx %d: power=%d\n",
			    __func__, c, ridx, idx);
			cmd.power[ridx].rf_gain[c] = rf_gain[idx];
			cmd.power[ridx].dsp_gain[c] = dsp_gain[idx];
		}
	}

	DPRINTF(sc, IWN_DEBUG_CALIBRATE | IWN_DEBUG_TXPOW,
	    "%s: set tx power for chan %d\n", __func__, chan);
	return iwn_cmd(sc, IWN_CMD_TXPOWER, &cmd, sizeof cmd, async);

#undef interpolate
#undef fdivround
}

static int
iwn5000_set_txpower(struct iwn_softc *sc, struct ieee80211_channel *ch,
    int async)
{
	struct iwn5000_cmd_txpower cmd;

	/*
	 * TX power calibration is handled automatically by the firmware
	 * for 5000 Series.
	 */
	memset(&cmd, 0, sizeof cmd);
	cmd.global_limit = 2 * IWN5000_TXPOWER_MAX_DBM;	/* 16 dBm */
	cmd.flags = IWN5000_TXPOWER_NO_CLOSED;
	cmd.srv_limit = IWN5000_TXPOWER_AUTO;
	DPRINTF(sc, IWN_DEBUG_CALIBRATE, "%s: setting TX power\n", __func__);
	return iwn_cmd(sc, IWN_CMD_TXPOWER_DBM, &cmd, sizeof cmd, async);
}

/*
 * Retrieve the maximum RSSI (in dBm) among receivers.
 */
static int
iwn4965_get_rssi(struct iwn_softc *sc, struct iwn_rx_stat *stat)
{
	struct iwn4965_rx_phystat *phy = (void *)stat->phybuf;
	uint8_t mask, agc;
	int rssi;

	mask = (le16toh(phy->antenna) >> 4) & IWN_ANT_ABC;
	agc  = (le16toh(phy->agc) >> 7) & 0x7f;

	rssi = 0;
#if 0
	if (mask & IWN_ANT_A)	/* Ant A */
		rssi = max(rssi, phy->rssi[0]);
	if (mask & IWN_ATH_B)	/* Ant B */
		rssi = max(rssi, phy->rssi[2]);
	if (mask & IWN_ANT_C)	/* Ant C */
		rssi = max(rssi, phy->rssi[4]);
#else
	rssi = max(rssi, phy->rssi[0]);
	rssi = max(rssi, phy->rssi[2]);
	rssi = max(rssi, phy->rssi[4]);
#endif

	DPRINTF(sc, IWN_DEBUG_RECV, "%s: agc %d mask 0x%x rssi %d %d %d "
	    "result %d\n", __func__, agc, mask,
	    phy->rssi[0], phy->rssi[2], phy->rssi[4],
	    rssi - agc - IWN_RSSI_TO_DBM);
	return rssi - agc - IWN_RSSI_TO_DBM;
}

static int
iwn5000_get_rssi(struct iwn_softc *sc, struct iwn_rx_stat *stat)
{
	struct iwn5000_rx_phystat *phy = (void *)stat->phybuf;
	int rssi;
	uint8_t agc;

	agc = (le32toh(phy->agc) >> 9) & 0x7f;

	rssi = MAX(le16toh(phy->rssi[0]) & 0xff,
		   le16toh(phy->rssi[1]) & 0xff);
	rssi = MAX(le16toh(phy->rssi[2]) & 0xff, rssi);

	DPRINTF(sc, IWN_DEBUG_RECV, "%s: agc %d rssi %d %d %d "
	    "result %d\n", __func__, agc,
	    phy->rssi[0], phy->rssi[1], phy->rssi[2],
	    rssi - agc - IWN_RSSI_TO_DBM);
	return rssi - agc - IWN_RSSI_TO_DBM;
}

/*
 * Retrieve the average noise (in dBm) among receivers.
 */
static int
iwn_get_noise(const struct iwn_rx_general_stats *stats)
{
	int i, total, nbant, noise;

	total = nbant = 0;
	for (i = 0; i < 3; i++) {
		if ((noise = le32toh(stats->noise[i]) & 0xff) == 0)
			continue;
		total += noise;
		nbant++;
	}
	/* There should be at least one antenna but check anyway. */
	return (nbant == 0) ? -127 : (total / nbant) - 107;
}

/*
 * Compute temperature (in degC) from last received statistics.
 */
static int
iwn4965_get_temperature(struct iwn_softc *sc)
{
	struct iwn_ucode_info *uc = &sc->ucode_info;
	int32_t r1, r2, r3, r4, temp;

	r1 = le32toh(uc->temp[0].chan20MHz);
	r2 = le32toh(uc->temp[1].chan20MHz);
	r3 = le32toh(uc->temp[2].chan20MHz);
	r4 = le32toh(sc->rawtemp);

	if (r1 == r3)	/* Prevents division by 0 (should not happen.) */
		return 0;

	/* Sign-extend 23-bit R4 value to 32-bit. */
	r4 = (r4 << 8) >> 8;
	/* Compute temperature in Kelvin. */
	temp = (259 * (r4 - r2)) / (r3 - r1);
	temp = (temp * 97) / 100 + 8;

	DPRINTF(sc, IWN_DEBUG_ANY, "temperature %dK/%dC\n", temp,
	    IWN_KTOC(temp));
	return IWN_KTOC(temp);
}

static int
iwn5000_get_temperature(struct iwn_softc *sc)
{
	int32_t temp;

	/*
	 * Temperature is not used by the driver for 5000 Series because
	 * TX power calibration is handled by firmware.  We export it to
	 * users through the sensor framework though.
	 */
	temp = le32toh(sc->rawtemp);
	if (sc->hw_type == IWN_HW_REV_TYPE_5150) {
		temp = (temp / -5) + sc->temp_off;
		temp = IWN_KTOC(temp);
	}
	return temp;
}

/*
 * Initialize sensitivity calibration state machine.
 */
static int
iwn_init_sensitivity(struct iwn_softc *sc)
{
	const struct iwn_hal *hal = sc->sc_hal;
	struct iwn_calib_state *calib = &sc->calib;
	uint32_t flags;
	int error;

	/* Reset calibration state machine. */
	memset(calib, 0, sizeof (*calib));
	calib->state = IWN_CALIB_STATE_INIT;
	calib->cck_state = IWN_CCK_STATE_HIFA;
	/* Set initial correlation values. */
	calib->ofdm_x1     = sc->limits->min_ofdm_x1;
	calib->ofdm_mrc_x1 = sc->limits->min_ofdm_mrc_x1;
	calib->ofdm_x4     = sc->limits->min_ofdm_x4;
	calib->ofdm_mrc_x4 = sc->limits->min_ofdm_mrc_x4;
	calib->cck_x4      = 125;
	calib->cck_mrc_x4  = sc->limits->min_cck_mrc_x4;
	calib->energy_cck  = sc->limits->energy_cck;

	/* Write initial sensitivity. */
	error = iwn_send_sensitivity(sc);
	if (error != 0)
		return error;

	/* Write initial gains. */
	error = hal->init_gains(sc);
	if (error != 0)
		return error;

	/* Request statistics at each beacon interval. */
	flags = 0;
	DPRINTF(sc, IWN_DEBUG_CALIBRATE, "%s: calibrate phy\n", __func__);
	return iwn_cmd(sc, IWN_CMD_GET_STATISTICS, &flags, sizeof flags, 1);
}

/*
 * Collect noise and RSSI statistics for the first 20 beacons received
 * after association and use them to determine connected antennas and
 * to set differential gains.
 */
static void
iwn_collect_noise(struct iwn_softc *sc,
    const struct iwn_rx_general_stats *stats)
{
	const struct iwn_hal *hal = sc->sc_hal;
	struct iwn_calib_state *calib = &sc->calib;
	uint32_t val;
	int i;

	/* Accumulate RSSI and noise for all 3 antennas. */
	for (i = 0; i < 3; i++) {
		calib->rssi[i] += le32toh(stats->rssi[i]) & 0xff;
		calib->noise[i] += le32toh(stats->noise[i]) & 0xff;
	}
	/* NB: We update differential gains only once after 20 beacons. */
	if (++calib->nbeacons < 20)
		return;

	/* Determine highest average RSSI. */
	val = MAX(calib->rssi[0], calib->rssi[1]);
	val = MAX(calib->rssi[2], val);

	/* Determine which antennas are connected. */
	sc->chainmask = 0;
	for (i = 0; i < 3; i++)
		if (val - calib->rssi[i] <= 15 * 20)
			sc->chainmask |= 1 << i;
	/* If none of the TX antennas are connected, keep at least one. */
	if ((sc->chainmask & sc->txchainmask) == 0)
		sc->chainmask |= IWN_LSB(sc->txchainmask);

	(void)hal->set_gains(sc);
	calib->state = IWN_CALIB_STATE_RUN;

#ifdef notyet
	/* XXX Disable RX chains with no antennas connected. */
	sc->rxon.rxchain = htole16(IWN_RXCHAIN_SEL(sc->chainmask));
	(void)iwn_cmd(sc, IWN_CMD_RXON, &sc->rxon, hal->rxonsz, 1);
#endif

#if 0
	/* XXX: not yet */
	/* Enable power-saving mode if requested by user. */
	if (sc->sc_ic.ic_flags & IEEE80211_F_PMGTON)
		(void)iwn_set_pslevel(sc, 0, 3, 1);
#endif
}

static int
iwn4965_init_gains(struct iwn_softc *sc)
{
	struct iwn_phy_calib_gain cmd;

	memset(&cmd, 0, sizeof cmd);
	cmd.code = IWN4965_PHY_CALIB_DIFF_GAIN;
	/* Differential gains initially set to 0 for all 3 antennas. */
	DPRINTF(sc, IWN_DEBUG_CALIBRATE,
	    "%s: setting initial differential gains\n", __func__);
	return iwn_cmd(sc, IWN_CMD_PHY_CALIB, &cmd, sizeof cmd, 1);
}

static int
iwn5000_init_gains(struct iwn_softc *sc)
{
	struct iwn_phy_calib cmd;

	memset(&cmd, 0, sizeof cmd);
	cmd.code = IWN5000_PHY_CALIB_RESET_NOISE_GAIN;
	cmd.ngroups = 1;
	cmd.isvalid = 1;
	DPRINTF(sc, IWN_DEBUG_CALIBRATE,
	    "%s: setting initial differential gains\n", __func__);
	return iwn_cmd(sc, IWN_CMD_PHY_CALIB, &cmd, sizeof cmd, 1);
}

static int
iwn4965_set_gains(struct iwn_softc *sc)
{
	struct iwn_calib_state *calib = &sc->calib;
	struct iwn_phy_calib_gain cmd;
	int i, delta, noise;

	/* Get minimal noise among connected antennas. */
	noise = INT_MAX;	/* NB: There's at least one antenna. */
	for (i = 0; i < 3; i++)
		if (sc->chainmask & (1 << i))
			noise = MIN(calib->noise[i], noise);

	memset(&cmd, 0, sizeof cmd);
	cmd.code = IWN4965_PHY_CALIB_DIFF_GAIN;
	/* Set differential gains for connected antennas. */
	for (i = 0; i < 3; i++) {
		if (sc->chainmask & (1 << i)) {
			/* Compute attenuation (in unit of 1.5dB). */
			delta = (noise - (int32_t)calib->noise[i]) / 30;
			/* NB: delta <= 0 */
			/* Limit to [-4.5dB,0]. */
			cmd.gain[i] = MIN(abs(delta), 3);
			if (delta < 0)
				cmd.gain[i] |= 1 << 2;	/* sign bit */
		}
	}
	DPRINTF(sc, IWN_DEBUG_CALIBRATE,
	    "setting differential gains Ant A/B/C: %x/%x/%x (%x)\n",
	    cmd.gain[0], cmd.gain[1], cmd.gain[2], sc->chainmask);
	return iwn_cmd(sc, IWN_CMD_PHY_CALIB, &cmd, sizeof cmd, 1);
}

static int
iwn5000_set_gains(struct iwn_softc *sc)
{
	struct iwn_calib_state *calib = &sc->calib;
	struct iwn_phy_calib_gain cmd;
	int i, ant, delta, div;

	/* We collected 20 beacons and !=6050 need a 1.5 factor. */
	div = (sc->hw_type == IWN_HW_REV_TYPE_6050) ? 20 : 30;

	memset(&cmd, 0, sizeof cmd);
	cmd.code = IWN5000_PHY_CALIB_NOISE_GAIN;
	cmd.ngroups = 1;
	cmd.isvalid = 1;
	/* Get first available RX antenna as referential. */
	ant = IWN_LSB(sc->rxchainmask);
	/* Set differential gains for other antennas. */
	for (i = ant + 1; i < 3; i++) {
		if (sc->chainmask & (1 << i)) {
			/* The delta is relative to antenna "ant". */
			delta = ((int32_t)calib->noise[ant] -
			    (int32_t)calib->noise[i]) / div;
			/* Limit to [-4.5dB,+4.5dB]. */
			cmd.gain[i - 1] = MIN(abs(delta), 3);
			if (delta < 0)
				cmd.gain[i - 1] |= 1 << 2;	/* sign bit */
		}
	}
	DPRINTF(sc, IWN_DEBUG_CALIBRATE,
	    "setting differential gains Ant B/C: %x/%x (%x)\n",
	    cmd.gain[0], cmd.gain[1], sc->chainmask);
	return iwn_cmd(sc, IWN_CMD_PHY_CALIB, &cmd, sizeof cmd, 1);
}

/*
 * Tune RF RX sensitivity based on the number of false alarms detected
 * during the last beacon period.
 */
static void
iwn_tune_sensitivity(struct iwn_softc *sc, const struct iwn_rx_stats *stats)
{
#define inc(val, inc, max)			\
	if ((val) < (max)) {			\
		if ((val) < (max) - (inc))	\
			(val) += (inc);		\
		else				\
			(val) = (max);		\
		needs_update = 1;		\
	}
#define dec(val, dec, min)			\
	if ((val) > (min)) {			\
		if ((val) > (min) + (dec))	\
			(val) -= (dec);		\
		else				\
			(val) = (min);		\
		needs_update = 1;		\
	}

	const struct iwn_sensitivity_limits *limits = sc->limits;
	struct iwn_calib_state *calib = &sc->calib;
	uint32_t val, rxena, fa;
	uint32_t energy[3], energy_min;
	uint8_t noise[3], noise_ref;
	int i, needs_update = 0;

	/* Check that we've been enabled long enough. */
	rxena = le32toh(stats->general.load);
	if (rxena == 0)
		return;

	/* Compute number of false alarms since last call for OFDM. */
	fa  = le32toh(stats->ofdm.bad_plcp) - calib->bad_plcp_ofdm;
	fa += le32toh(stats->ofdm.fa) - calib->fa_ofdm;
	fa *= 200 * 1024;	/* 200TU */

	/* Save counters values for next call. */
	calib->bad_plcp_ofdm = le32toh(stats->ofdm.bad_plcp);
	calib->fa_ofdm = le32toh(stats->ofdm.fa);

	if (fa > 50 * rxena) {
		/* High false alarm count, decrease sensitivity. */
		DPRINTF(sc, IWN_DEBUG_CALIBRATE,
		    "%s: OFDM high false alarm count: %u\n", __func__, fa);
		inc(calib->ofdm_x1,     1, limits->max_ofdm_x1);
		inc(calib->ofdm_mrc_x1, 1, limits->max_ofdm_mrc_x1);
		inc(calib->ofdm_x4,     1, limits->max_ofdm_x4);
		inc(calib->ofdm_mrc_x4, 1, limits->max_ofdm_mrc_x4);

	} else if (fa < 5 * rxena) {
		/* Low false alarm count, increase sensitivity. */
		DPRINTF(sc, IWN_DEBUG_CALIBRATE,
		    "%s: OFDM low false alarm count: %u\n", __func__, fa);
		dec(calib->ofdm_x1,     1, limits->min_ofdm_x1);
		dec(calib->ofdm_mrc_x1, 1, limits->min_ofdm_mrc_x1);
		dec(calib->ofdm_x4,     1, limits->min_ofdm_x4);
		dec(calib->ofdm_mrc_x4, 1, limits->min_ofdm_mrc_x4);
	}

	/* Compute maximum noise among 3 receivers. */
	for (i = 0; i < 3; i++)
		noise[i] = (le32toh(stats->general.noise[i]) >> 8) & 0xff;
	val = MAX(noise[0], noise[1]);
	val = MAX(noise[2], val);
	/* Insert it into our samples table. */
	calib->noise_samples[calib->cur_noise_sample] = val;
	calib->cur_noise_sample = (calib->cur_noise_sample + 1) % 20;

	/* Compute maximum noise among last 20 samples. */
	noise_ref = calib->noise_samples[0];
	for (i = 1; i < 20; i++)
		noise_ref = MAX(noise_ref, calib->noise_samples[i]);

	/* Compute maximum energy among 3 receivers. */
	for (i = 0; i < 3; i++)
		energy[i] = le32toh(stats->general.energy[i]);
	val = MIN(energy[0], energy[1]);
	val = MIN(energy[2], val);
	/* Insert it into our samples table. */
	calib->energy_samples[calib->cur_energy_sample] = val;
	calib->cur_energy_sample = (calib->cur_energy_sample + 1) % 10;

	/* Compute minimum energy among last 10 samples. */
	energy_min = calib->energy_samples[0];
	for (i = 1; i < 10; i++)
		energy_min = MAX(energy_min, calib->energy_samples[i]);
	energy_min += 6;

	/* Compute number of false alarms since last call for CCK. */
	fa  = le32toh(stats->cck.bad_plcp) - calib->bad_plcp_cck;
	fa += le32toh(stats->cck.fa) - calib->fa_cck;
	fa *= 200 * 1024;	/* 200TU */

	/* Save counters values for next call. */
	calib->bad_plcp_cck = le32toh(stats->cck.bad_plcp);
	calib->fa_cck = le32toh(stats->cck.fa);

	if (fa > 50 * rxena) {
		/* High false alarm count, decrease sensitivity. */
		DPRINTF(sc, IWN_DEBUG_CALIBRATE,
		    "%s: CCK high false alarm count: %u\n", __func__, fa);
		calib->cck_state = IWN_CCK_STATE_HIFA;
		calib->low_fa = 0;

		if (calib->cck_x4 > 160) {
			calib->noise_ref = noise_ref;
			if (calib->energy_cck > 2)
				dec(calib->energy_cck, 2, energy_min);
		}
		if (calib->cck_x4 < 160) {
			calib->cck_x4 = 161;
			needs_update = 1;
		} else
			inc(calib->cck_x4, 3, limits->max_cck_x4);

		inc(calib->cck_mrc_x4, 3, limits->max_cck_mrc_x4);

	} else if (fa < 5 * rxena) {
		/* Low false alarm count, increase sensitivity. */
		DPRINTF(sc, IWN_DEBUG_CALIBRATE,
		    "%s: CCK low false alarm count: %u\n", __func__, fa);
		calib->cck_state = IWN_CCK_STATE_LOFA;
		calib->low_fa++;

		if (calib->cck_state != IWN_CCK_STATE_INIT &&
		    (((int32_t)calib->noise_ref - (int32_t)noise_ref) > 2 ||
		    calib->low_fa > 100)) {
			inc(calib->energy_cck, 2, limits->min_energy_cck);
			dec(calib->cck_x4,     3, limits->min_cck_x4);
			dec(calib->cck_mrc_x4, 3, limits->min_cck_mrc_x4);
		}
	} else {
		/* Not worth to increase or decrease sensitivity. */
		DPRINTF(sc, IWN_DEBUG_CALIBRATE,
		    "%s: CCK normal false alarm count: %u\n", __func__, fa);
		calib->low_fa = 0;
		calib->noise_ref = noise_ref;

		if (calib->cck_state == IWN_CCK_STATE_HIFA) {
			/* Previous interval had many false alarms. */
			dec(calib->energy_cck, 8, energy_min);
		}
		calib->cck_state = IWN_CCK_STATE_INIT;
	}

	if (needs_update)
		(void)iwn_send_sensitivity(sc);
#undef dec
#undef inc
}

static int
iwn_send_sensitivity(struct iwn_softc *sc)
{
	struct iwn_calib_state *calib = &sc->calib;
	struct iwn_sensitivity_cmd cmd;

	memset(&cmd, 0, sizeof cmd);
	cmd.which = IWN_SENSITIVITY_WORKTBL;
	/* OFDM modulation. */
	cmd.corr_ofdm_x1     = htole16(calib->ofdm_x1);
	cmd.corr_ofdm_mrc_x1 = htole16(calib->ofdm_mrc_x1);
	cmd.corr_ofdm_x4     = htole16(calib->ofdm_x4);
	cmd.corr_ofdm_mrc_x4 = htole16(calib->ofdm_mrc_x4);
	cmd.energy_ofdm      = htole16(sc->limits->energy_ofdm);
	cmd.energy_ofdm_th   = htole16(62);
	/* CCK modulation. */
	cmd.corr_cck_x4      = htole16(calib->cck_x4);
	cmd.corr_cck_mrc_x4  = htole16(calib->cck_mrc_x4);
	cmd.energy_cck       = htole16(calib->energy_cck);
	/* Barker modulation: use default values. */
	cmd.corr_barker      = htole16(190);
	cmd.corr_barker_mrc  = htole16(390);

	DPRINTF(sc, IWN_DEBUG_CALIBRATE,
	    "%s: set sensitivity %d/%d/%d/%d/%d/%d/%d\n", __func__,
	    calib->ofdm_x1, calib->ofdm_mrc_x1, calib->ofdm_x4,
	    calib->ofdm_mrc_x4, calib->cck_x4,
	    calib->cck_mrc_x4, calib->energy_cck);
	return iwn_cmd(sc, IWN_CMD_SET_SENSITIVITY, &cmd, sizeof cmd, 1);
}

/*
 * Set STA mode power saving level (between 0 and 5).
 * Level 0 is CAM (Continuously Aware Mode), 5 is for maximum power saving.
 */
static int
iwn_set_pslevel(struct iwn_softc *sc, int dtim, int level, int async)
{
	const struct iwn_pmgt *pmgt;
	struct iwn_pmgt_cmd cmd;
	uint32_t max, skip_dtim;
	uint32_t tmp;
	int i;

	/* Select which PS parameters to use. */
	if (dtim <= 2)
		pmgt = &iwn_pmgt[0][level];
	else if (dtim <= 10)
		pmgt = &iwn_pmgt[1][level];
	else
		pmgt = &iwn_pmgt[2][level];

	memset(&cmd, 0, sizeof cmd);
	if (level != 0)	/* not CAM */
		cmd.flags |= htole16(IWN_PS_ALLOW_SLEEP);
	if (level == 5)
		cmd.flags |= htole16(IWN_PS_FAST_PD);
	/* Retrieve PCIe Active State Power Management (ASPM). */
	tmp = pci_read_config(sc->sc_dev, sc->sc_cap_off + 0x10, 1);
	if (!(tmp & 0x1))	/* L0s Entry disabled. */
		cmd.flags |= htole16(IWN_PS_PCI_PMGT);
	cmd.rxtimeout = htole32(pmgt->rxtimeout * 1024);
	cmd.txtimeout = htole32(pmgt->txtimeout * 1024);

	if (dtim == 0) {
		dtim = 1;
		skip_dtim = 0;
	} else
		skip_dtim = pmgt->skip_dtim;
	if (skip_dtim != 0) {
		cmd.flags |= htole16(IWN_PS_SLEEP_OVER_DTIM);
		max = pmgt->intval[4];
		if (max == (uint32_t)-1)
			max = dtim * (skip_dtim + 1);
		else if (max > dtim)
			max = (max / dtim) * dtim;
	} else
		max = dtim;
	for (i = 0; i < 5; i++)
		cmd.intval[i] = htole32(MIN(max, pmgt->intval[i]));

	DPRINTF(sc, IWN_DEBUG_RESET, "setting power saving level to %d\n",
	    level);
	return iwn_cmd(sc, IWN_CMD_SET_POWER_MODE, &cmd, sizeof cmd, async);
}

static int
iwn_config(struct iwn_softc *sc)
{
	const struct iwn_hal *hal = sc->sc_hal;
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	struct iwn_bluetooth bluetooth;
	uint32_t txmask;
	int error;
	uint16_t rxchain;

	/* Configure valid TX chains for 5000 Series. */
	if (sc->hw_type != IWN_HW_REV_TYPE_4965) {
		txmask = htole32(sc->txchainmask);
		DPRINTF(sc, IWN_DEBUG_RESET,
		    "%s: configuring valid TX chains 0x%x\n", __func__, txmask);
		error = iwn_cmd(sc, IWN5000_CMD_TX_ANT_CONFIG, &txmask,
		    sizeof txmask, 0);
		if (error != 0) {
			device_printf(sc->sc_dev,
			    "%s: could not configure valid TX chains, "
			    "error %d\n", __func__, error);
			return error;
		}
	}

	/* Configure bluetooth coexistence. */
	memset(&bluetooth, 0, sizeof bluetooth);
	bluetooth.flags = IWN_BT_COEX_CHAN_ANN | IWN_BT_COEX_BT_PRIO;
	bluetooth.lead_time = IWN_BT_LEAD_TIME_DEF;
	bluetooth.max_kill = IWN_BT_MAX_KILL_DEF;
	DPRINTF(sc, IWN_DEBUG_RESET, "%s: config bluetooth coexistence\n",
	    __func__);
	error = iwn_cmd(sc, IWN_CMD_BT_COEX, &bluetooth, sizeof bluetooth, 0);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not configure bluetooth coexistence, error %d\n",
		    __func__, error);
		return error;
	}

	/* Set mode, channel, RX filter and enable RX. */
	memset(&sc->rxon, 0, sizeof (struct iwn_rxon));
	IEEE80211_ADDR_COPY(sc->rxon.myaddr, IF_LLADDR(ifp));
	IEEE80211_ADDR_COPY(sc->rxon.wlap, IF_LLADDR(ifp));
	sc->rxon.chan = ieee80211_chan2ieee(ic, ic->ic_curchan);
	sc->rxon.flags = htole32(IWN_RXON_TSF | IWN_RXON_CTS_TO_SELF);
	if (IEEE80211_IS_CHAN_2GHZ(ic->ic_curchan))
		sc->rxon.flags |= htole32(IWN_RXON_AUTO | IWN_RXON_24GHZ);
	switch (ic->ic_opmode) {
	case IEEE80211_M_STA:
		sc->rxon.mode = IWN_MODE_STA;
		sc->rxon.filter = htole32(IWN_FILTER_MULTICAST);
		break;
	case IEEE80211_M_MONITOR:
		sc->rxon.mode = IWN_MODE_MONITOR;
		sc->rxon.filter = htole32(IWN_FILTER_MULTICAST |
		    IWN_FILTER_CTL | IWN_FILTER_PROMISC);
		break;
	default:
		/* Should not get there. */
		break;
	}
	sc->rxon.cck_mask  = 0x0f;	/* not yet negotiated */
	sc->rxon.ofdm_mask = 0xff;	/* not yet negotiated */
	sc->rxon.ht_single_mask = 0xff;
	sc->rxon.ht_dual_mask = 0xff;
	sc->rxon.ht_triple_mask = 0xff;
	rxchain =
	    IWN_RXCHAIN_VALID(sc->rxchainmask) |
	    IWN_RXCHAIN_MIMO_COUNT(2) |
	    IWN_RXCHAIN_IDLE_COUNT(2);
	sc->rxon.rxchain = htole16(rxchain);
	DPRINTF(sc, IWN_DEBUG_RESET, "%s: setting configuration\n", __func__);
	error = iwn_cmd(sc, IWN_CMD_RXON, &sc->rxon, hal->rxonsz, 0);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: RXON command failed\n", __func__);
		return error;
	}

	error = iwn_add_broadcast_node(sc, 0);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not add broadcast node\n", __func__);
		return error;
	}

	/* Configuration has changed, set TX power accordingly. */
	error = hal->set_txpower(sc, ic->ic_curchan, 0);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not set TX power\n", __func__);
		return error;
	}

	error = iwn_set_critical_temp(sc);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: ccould not set critical temperature\n", __func__);
		return error;
	}

	/* Set power saving level to CAM during initialization. */
	error = iwn_set_pslevel(sc, 0, 0, 0);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not set power saving level\n", __func__);
		return error;
	}
	return 0;
}

static int
iwn_scan(struct iwn_softc *sc)
{
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	struct ieee80211_scan_state *ss = ic->ic_scan;	/*XXX*/
	struct iwn_scan_hdr *hdr;
	struct iwn_cmd_data *tx;
	struct iwn_scan_essid *essid;
	struct iwn_scan_chan *chan;
	struct ieee80211_frame *wh;
	struct ieee80211_rateset *rs;
	struct ieee80211_channel *c;
	int buflen, error, nrates;
	uint16_t rxchain;
	uint8_t *buf, *frm, txant;

	buf = malloc(IWN_SCAN_MAXSZ, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (buf == NULL) {
		device_printf(sc->sc_dev,
		    "%s: could not allocate buffer for scan command\n",
		    __func__);
		return ENOMEM;
	}
	hdr = (struct iwn_scan_hdr *)buf;

	/*
	 * Move to the next channel if no frames are received within 10ms
	 * after sending the probe request.
	 */
	hdr->quiet_time = htole16(10);		/* timeout in milliseconds */
	hdr->quiet_threshold = htole16(1);	/* min # of packets */

	/* Select antennas for scanning. */
	rxchain =
	    IWN_RXCHAIN_VALID(sc->rxchainmask) |
	    IWN_RXCHAIN_FORCE_MIMO_SEL(sc->rxchainmask) |
	    IWN_RXCHAIN_DRIVER_FORCE;
	if (IEEE80211_IS_CHAN_A(ic->ic_curchan) &&
	    sc->hw_type == IWN_HW_REV_TYPE_4965) {
		/* Ant A must be avoided in 5GHz because of an HW bug. */
		rxchain |= IWN_RXCHAIN_FORCE_SEL(IWN_ANT_BC);
	} else	/* Use all available RX antennas. */
		rxchain |= IWN_RXCHAIN_FORCE_SEL(sc->rxchainmask);
	hdr->rxchain = htole16(rxchain);
	hdr->filter = htole32(IWN_FILTER_MULTICAST | IWN_FILTER_BEACON);

	tx = (struct iwn_cmd_data *)(hdr + 1);
	tx->flags = htole32(IWN_TX_AUTO_SEQ);
	tx->id = sc->sc_hal->broadcast_id;
	tx->lifetime = htole32(IWN_LIFETIME_INFINITE);

	if (IEEE80211_IS_CHAN_A(ic->ic_curchan)) {
		/* Send probe requests at 6Mbps. */
		tx->plcp = iwn_rates[IWN_RIDX_OFDM6].plcp;
		rs = &ic->ic_sup_rates[IEEE80211_MODE_11A];
	} else {
		hdr->flags = htole32(IWN_RXON_24GHZ | IWN_RXON_AUTO);
		/* Send probe requests at 1Mbps. */
		tx->plcp = iwn_rates[IWN_RIDX_CCK1].plcp;
		tx->rflags = IWN_RFLAG_CCK;
		rs = &ic->ic_sup_rates[IEEE80211_MODE_11G];
	}
	/* Use the first valid TX antenna. */
	txant = IWN_LSB(sc->txchainmask);
	tx->rflags |= IWN_RFLAG_ANT(txant);

	essid = (struct iwn_scan_essid *)(tx + 1);
	if (ss->ss_ssid[0].len != 0) {
		essid[0].id = IEEE80211_ELEMID_SSID;
		essid[0].len = ss->ss_ssid[0].len;
		memcpy(essid[0].data, ss->ss_ssid[0].ssid, ss->ss_ssid[0].len);
	}

	/*
	 * Build a probe request frame.  Most of the following code is a
	 * copy & paste of what is done in net80211.
	 */
	wh = (struct ieee80211_frame *)(essid + 20);
	wh->i_fc[0] = IEEE80211_FC0_VERSION_0 | IEEE80211_FC0_TYPE_MGT |
	    IEEE80211_FC0_SUBTYPE_PROBE_REQ;
	wh->i_fc[1] = IEEE80211_FC1_DIR_NODS;
	IEEE80211_ADDR_COPY(wh->i_addr1, ifp->if_broadcastaddr);
	IEEE80211_ADDR_COPY(wh->i_addr2, IF_LLADDR(ifp));
	IEEE80211_ADDR_COPY(wh->i_addr3, ifp->if_broadcastaddr);
	*(uint16_t *)&wh->i_dur[0] = 0;	/* filled by HW */
	*(uint16_t *)&wh->i_seq[0] = 0;	/* filled by HW */

	frm = (uint8_t *)(wh + 1);

	/* Add SSID IE. */
	*frm++ = IEEE80211_ELEMID_SSID;
	*frm++ = ss->ss_ssid[0].len;
	memcpy(frm, ss->ss_ssid[0].ssid, ss->ss_ssid[0].len);
	frm += ss->ss_ssid[0].len;

	/* Add supported rates IE. */
	*frm++ = IEEE80211_ELEMID_RATES;
	nrates = rs->rs_nrates;
	if (nrates > IEEE80211_RATE_SIZE)
		nrates = IEEE80211_RATE_SIZE;
	*frm++ = nrates;
	memcpy(frm, rs->rs_rates, nrates);
	frm += nrates;

	/* Add supported xrates IE. */
	if (rs->rs_nrates > IEEE80211_RATE_SIZE) {
		nrates = rs->rs_nrates - IEEE80211_RATE_SIZE;
		*frm++ = IEEE80211_ELEMID_XRATES;
		*frm++ = (uint8_t)nrates;
		memcpy(frm, rs->rs_rates + IEEE80211_RATE_SIZE, nrates);
		frm += nrates;
	}

	/* Set length of probe request. */
	tx->len = htole16(frm - (uint8_t *)wh);

	c = ic->ic_curchan;
	chan = (struct iwn_scan_chan *)frm;
	chan->chan = htole16(ieee80211_chan2ieee(ic, c));
	chan->flags = 0;
	if (ss->ss_nssid > 0)
		chan->flags |= htole32(IWN_CHAN_NPBREQS(1));
	chan->dsp_gain = 0x6e;
	if (IEEE80211_IS_CHAN_5GHZ(c) &&
	    !(c->ic_flags & IEEE80211_CHAN_PASSIVE)) {
		chan->rf_gain = 0x3b;
		chan->active  = htole16(24);
		chan->passive = htole16(110);
		chan->flags |= htole32(IWN_CHAN_ACTIVE);
	} else if (IEEE80211_IS_CHAN_5GHZ(c)) {
		chan->rf_gain = 0x3b;
		chan->active  = htole16(24);
		if (sc->rxon.associd)
			chan->passive = htole16(78);
		else
			chan->passive = htole16(110);
		hdr->crc_threshold = htole16(1);
	} else if (!(c->ic_flags & IEEE80211_CHAN_PASSIVE)) {
		chan->rf_gain = 0x28;
		chan->active  = htole16(36);
		chan->passive = htole16(120);
		chan->flags |= htole32(IWN_CHAN_ACTIVE);
	} else {
		chan->rf_gain = 0x28;
		chan->active  = htole16(36);
		if (sc->rxon.associd)
			chan->passive = htole16(88);
		else
			chan->passive = htole16(120);
		hdr->crc_threshold = htole16(1);
	}

	DPRINTF(sc, IWN_DEBUG_STATE,
	    "%s: chan %u flags 0x%x rf_gain 0x%x "
	    "dsp_gain 0x%x active 0x%x passive 0x%x\n", __func__,
	    chan->chan, chan->flags, chan->rf_gain, chan->dsp_gain,
	    chan->active, chan->passive);

	hdr->nchan++;
	chan++;
	buflen = (uint8_t *)chan - buf;
	hdr->len = htole16(buflen);

	DPRINTF(sc, IWN_DEBUG_STATE, "sending scan command nchan=%d\n",
	    hdr->nchan);
	error = iwn_cmd(sc, IWN_CMD_SCAN, buf, buflen, 1);
	free(buf, M_DEVBUF);
	return error;
}

static int
iwn_auth(struct iwn_softc *sc, struct ieee80211vap *vap)
{
	const struct iwn_hal *hal = sc->sc_hal;
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	struct ieee80211_node *ni = vap->iv_bss;
	int error;

	sc->calib.state = IWN_CALIB_STATE_INIT;

	/* Update adapter configuration. */
	IEEE80211_ADDR_COPY(sc->rxon.bssid, ni->ni_bssid);
	sc->rxon.chan = htole16(ieee80211_chan2ieee(ic, ni->ni_chan));
	sc->rxon.flags = htole32(IWN_RXON_TSF | IWN_RXON_CTS_TO_SELF);
	if (IEEE80211_IS_CHAN_2GHZ(ni->ni_chan))
		sc->rxon.flags |= htole32(IWN_RXON_AUTO | IWN_RXON_24GHZ);
	if (ic->ic_flags & IEEE80211_F_SHSLOT)
		sc->rxon.flags |= htole32(IWN_RXON_SHSLOT);
	if (ic->ic_flags & IEEE80211_F_SHPREAMBLE)
		sc->rxon.flags |= htole32(IWN_RXON_SHPREAMBLE);
	if (IEEE80211_IS_CHAN_A(ni->ni_chan)) {
		sc->rxon.cck_mask  = 0;
		sc->rxon.ofdm_mask = 0x15;
	} else if (IEEE80211_IS_CHAN_B(ni->ni_chan)) {
		sc->rxon.cck_mask  = 0x03;
		sc->rxon.ofdm_mask = 0;
	} else {
		/* XXX assume 802.11b/g */
		sc->rxon.cck_mask  = 0x0f;
		sc->rxon.ofdm_mask = 0x15;
	}
	DPRINTF(sc, IWN_DEBUG_STATE,
	    "%s: config chan %d mode %d flags 0x%x cck 0x%x ofdm 0x%x "
	    "ht_single 0x%x ht_dual 0x%x rxchain 0x%x "
	    "myaddr %6D wlap %6D bssid %6D associd %d filter 0x%x\n",
	    __func__,
	    le16toh(sc->rxon.chan), sc->rxon.mode, le32toh(sc->rxon.flags),
	    sc->rxon.cck_mask, sc->rxon.ofdm_mask,
	    sc->rxon.ht_single_mask, sc->rxon.ht_dual_mask,
	    le16toh(sc->rxon.rxchain),
	    sc->rxon.myaddr, ":", sc->rxon.wlap, ":", sc->rxon.bssid, ":",
	    le16toh(sc->rxon.associd), le32toh(sc->rxon.filter));
	error = iwn_cmd(sc, IWN_CMD_RXON, &sc->rxon, hal->rxonsz, 1);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: RXON command failed, error %d\n", __func__, error);
		return error;
	}

	/* Configuration has changed, set TX power accordingly. */
	error = hal->set_txpower(sc, ni->ni_chan, 1);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not set Tx power, error %d\n", __func__, error);
		return error;
	}
	/*
	 * Reconfiguring RXON clears the firmware nodes table so we must
	 * add the broadcast node again.
	 */
	error = iwn_add_broadcast_node(sc, 1);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not add broadcast node, error %d\n",
		    __func__, error);
		return error;
	}
	return 0;
}

/*
 * Configure the adapter for associated state.
 */
static int
iwn_run(struct iwn_softc *sc, struct ieee80211vap *vap)
{
#define	MS(v,x)	(((v) & x) >> x##_S)
	const struct iwn_hal *hal = sc->sc_hal;
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	struct ieee80211_node *ni = vap->iv_bss;
	struct iwn_node_info node;
	int error;

	sc->calib.state = IWN_CALIB_STATE_INIT;

	if (ic->ic_opmode == IEEE80211_M_MONITOR) {
		/* Link LED blinks while monitoring. */
		iwn_set_led(sc, IWN_LED_LINK, 5, 5);
		return 0;
	}
	error = iwn_set_timing(sc, ni);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not set timing, error %d\n", __func__, error);
		return error;
	}

	/* Update adapter configuration. */
	IEEE80211_ADDR_COPY(sc->rxon.bssid, ni->ni_bssid);
	sc->rxon.chan = htole16(ieee80211_chan2ieee(ic, ni->ni_chan));
	sc->rxon.associd = htole16(IEEE80211_AID(ni->ni_associd));
	/* Short preamble and slot time are negotiated when associating. */
	sc->rxon.flags &= ~htole32(IWN_RXON_SHPREAMBLE | IWN_RXON_SHSLOT);
	sc->rxon.flags |= htole32(IWN_RXON_TSF | IWN_RXON_CTS_TO_SELF);
	if (IEEE80211_IS_CHAN_2GHZ(ni->ni_chan))
		sc->rxon.flags |= htole32(IWN_RXON_AUTO | IWN_RXON_24GHZ);
	else
		sc->rxon.flags &= ~htole32(IWN_RXON_AUTO | IWN_RXON_24GHZ);
	if (ic->ic_flags & IEEE80211_F_SHSLOT)
		sc->rxon.flags |= htole32(IWN_RXON_SHSLOT);
	if (ic->ic_flags & IEEE80211_F_SHPREAMBLE)
		sc->rxon.flags |= htole32(IWN_RXON_SHPREAMBLE);
	if (IEEE80211_IS_CHAN_A(ni->ni_chan)) {
		sc->rxon.cck_mask  = 0;
		sc->rxon.ofdm_mask = 0x15;
	} else if (IEEE80211_IS_CHAN_B(ni->ni_chan)) {
		sc->rxon.cck_mask  = 0x03;
		sc->rxon.ofdm_mask = 0;
	} else {
		/* XXX assume 802.11b/g */
		sc->rxon.cck_mask  = 0x0f;
		sc->rxon.ofdm_mask = 0x15;
	}
#if 0	/* HT */
	if (IEEE80211_IS_CHAN_HT(ni->ni_chan)) {
		sc->rxon.flags &= ~htole32(IWN_RXON_HT);
		if (IEEE80211_IS_CHAN_HT40U(ni->ni_chan))
			sc->rxon.flags |= htole32(IWN_RXON_HT40U);
		else if (IEEE80211_IS_CHAN_HT40D(ni->ni_chan))
			sc->rxon.flags |= htole32(IWN_RXON_HT40D);
		else
			sc->rxon.flags |= htole32(IWN_RXON_HT20);
		sc->rxon.rxchain = htole16(
			  IWN_RXCHAIN_VALID(3)
			| IWN_RXCHAIN_MIMO_COUNT(3)
			| IWN_RXCHAIN_IDLE_COUNT(1)
			| IWN_RXCHAIN_MIMO_FORCE);

		maxrxampdu = MS(ni->ni_htparam, IEEE80211_HTCAP_MAXRXAMPDU);
		ampdudensity = MS(ni->ni_htparam, IEEE80211_HTCAP_MPDUDENSITY);
	} else
		maxrxampdu = ampdudensity = 0;
#endif
	sc->rxon.filter |= htole32(IWN_FILTER_BSS);

	DPRINTF(sc, IWN_DEBUG_STATE,
	    "%s: config chan %d mode %d flags 0x%x cck 0x%x ofdm 0x%x "
	    "ht_single 0x%x ht_dual 0x%x rxchain 0x%x "
	    "myaddr %6D wlap %6D bssid %6D associd %d filter 0x%x\n",
	    __func__,
	    le16toh(sc->rxon.chan), sc->rxon.mode, le32toh(sc->rxon.flags),
	    sc->rxon.cck_mask, sc->rxon.ofdm_mask,
	    sc->rxon.ht_single_mask, sc->rxon.ht_dual_mask,
	    le16toh(sc->rxon.rxchain),
	    sc->rxon.myaddr, ":", sc->rxon.wlap, ":", sc->rxon.bssid, ":",
	    le16toh(sc->rxon.associd), le32toh(sc->rxon.filter));
	error = iwn_cmd(sc, IWN_CMD_RXON, &sc->rxon, hal->rxonsz, 1);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not update configuration, error %d\n",
		    __func__, error);
		return error;
	}

	/* Configuration has changed, set TX power accordingly. */
	error = hal->set_txpower(sc, ni->ni_chan, 1);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not set Tx power, error %d\n", __func__, error);
		return error;
	}

	/* Add BSS node. */
	memset(&node, 0, sizeof node);
	IEEE80211_ADDR_COPY(node.macaddr, ni->ni_macaddr);
	node.id = IWN_ID_BSS;
#ifdef notyet
	node.htflags = htole32(IWN_AMDPU_SIZE_FACTOR(3) |
	    IWN_AMDPU_DENSITY(5));	/* 2us */
#endif
	DPRINTF(sc, IWN_DEBUG_STATE, "%s: add BSS node, id %d htflags 0x%x\n",
	    __func__, node.id, le32toh(node.htflags));
	error = hal->add_node(sc, &node, 1);
	if (error != 0) {
		device_printf(sc->sc_dev, "could not add BSS node\n");
		return error;
	}
	DPRINTF(sc, IWN_DEBUG_STATE, "setting link quality for node %d\n",
	    node.id);
	error = iwn_set_link_quality(sc, node.id, 1);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not setup MRR for node %d, error %d\n",
		    __func__, node.id, error);
		return error;
	}

	error = iwn_init_sensitivity(sc);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not set sensitivity, error %d\n",
		    __func__, error);
		return error;
	}

	/* Start periodic calibration timer. */
	sc->calib.state = IWN_CALIB_STATE_ASSOC;
	iwn_calib_reset(sc);

	/* Link LED always on while associated. */
	iwn_set_led(sc, IWN_LED_LINK, 0, 1);

	return 0;
#undef MS
}

#if 0	/* HT */
/*
 * This function is called by upper layer when an ADDBA request is received
 * from another STA and before the ADDBA response is sent.
 */
static int
iwn_ampdu_rx_start(struct ieee80211com *ic, struct ieee80211_node *ni,
    uint8_t tid)
{
	struct ieee80211_rx_ba *ba = &ni->ni_rx_ba[tid];
	struct iwn_softc *sc = ic->ic_softc;
	struct iwn_node *wn = (void *)ni;
	struct iwn_node_info node;

	memset(&node, 0, sizeof node);
	node.id = wn->id;
	node.control = IWN_NODE_UPDATE;
	node.flags = IWN_FLAG_SET_ADDBA;
	node.addba_tid = tid;
	node.addba_ssn = htole16(ba->ba_winstart);
	DPRINTF(sc, IWN_DEBUG_RECV, "ADDBA RA=%d TID=%d SSN=%d\n",
	    wn->id, tid, ba->ba_winstart));
	return sc->sc_hal->add_node(sc, &node, 1);
}

/*
 * This function is called by upper layer on teardown of an HT-immediate
 * Block Ack agreement (eg. uppon receipt of a DELBA frame.)
 */
static void
iwn_ampdu_rx_stop(struct ieee80211com *ic, struct ieee80211_node *ni,
    uint8_t tid)
{
	struct iwn_softc *sc = ic->ic_softc;
	struct iwn_node *wn = (void *)ni;
	struct iwn_node_info node;

	memset(&node, 0, sizeof node);
	node.id = wn->id;
	node.control = IWN_NODE_UPDATE;
	node.flags = IWN_FLAG_SET_DELBA;
	node.delba_tid = tid;
	DPRINTF(sc, IWN_DEBUG_RECV, "DELBA RA=%d TID=%d\n", wn->id, tid);
	(void)sc->sc_hal->add_node(sc, &node, 1);
}

/*
 * This function is called by upper layer when an ADDBA response is received
 * from another STA.
 */
static int
iwn_ampdu_tx_start(struct ieee80211com *ic, struct ieee80211_node *ni,
    uint8_t tid)
{
	struct ieee80211_tx_ba *ba = &ni->ni_tx_ba[tid];
	struct iwn_softc *sc = ic->ic_softc;
	const struct iwn_hal *hal = sc->sc_hal;
	struct iwn_node *wn = (void *)ni;
	struct iwn_node_info node;
	int error;

	/* Enable TX for the specified RA/TID. */
	wn->disable_tid &= ~(1 << tid);
	memset(&node, 0, sizeof node);
	node.id = wn->id;
	node.control = IWN_NODE_UPDATE;
	node.flags = IWN_FLAG_SET_DISABLE_TID;
	node.disable_tid = htole16(wn->disable_tid);
	error = hal->add_node(sc, &node, 1);
	if (error != 0)
		return error;

	if ((error = iwn_nic_lock(sc)) != 0)
		return error;
	hal->ampdu_tx_start(sc, ni, tid, ba->ba_winstart);
	iwn_nic_unlock(sc);
	return 0;
}

static void
iwn_ampdu_tx_stop(struct ieee80211com *ic, struct ieee80211_node *ni,
    uint8_t tid)
{
	struct ieee80211_tx_ba *ba = &ni->ni_tx_ba[tid];
	struct iwn_softc *sc = ic->ic_softc;
	int error;

	error = iwn_nic_lock(sc);
	if (error != 0)
		return;
	sc->sc_hal->ampdu_tx_stop(sc, tid, ba->ba_winstart);
	iwn_nic_unlock(sc);
}

static void
iwn4965_ampdu_tx_start(struct iwn_softc *sc, struct ieee80211_node *ni,
    uint8_t tid, uint16_t ssn)
{
	struct iwn_node *wn = (void *)ni;
	int qid = 7 + tid;

	/* Stop TX scheduler while we're changing its configuration. */
	iwn_prph_write(sc, IWN4965_SCHED_QUEUE_STATUS(qid),
	    IWN4965_TXQ_STATUS_CHGACT);

	/* Assign RA/TID translation to the queue. */
	iwn_mem_write_2(sc, sc->sched_base + IWN4965_SCHED_TRANS_TBL(qid),
	    wn->id << 4 | tid);

	/* Enable chain-building mode for the queue. */
	iwn_prph_setbits(sc, IWN4965_SCHED_QCHAIN_SEL, 1 << qid);

	/* Set starting sequence number from the ADDBA request. */
	IWN_WRITE(sc, IWN_HBUS_TARG_WRPTR, qid << 8 | (ssn & 0xff));
	iwn_prph_write(sc, IWN4965_SCHED_QUEUE_RDPTR(qid), ssn);

	/* Set scheduler window size. */
	iwn_mem_write(sc, sc->sched_base + IWN4965_SCHED_QUEUE_OFFSET(qid),
	    IWN_SCHED_WINSZ);
	/* Set scheduler frame limit. */
	iwn_mem_write(sc, sc->sched_base + IWN4965_SCHED_QUEUE_OFFSET(qid) + 4,
	    IWN_SCHED_LIMIT << 16);

	/* Enable interrupts for the queue. */
	iwn_prph_setbits(sc, IWN4965_SCHED_INTR_MASK, 1 << qid);

	/* Mark the queue as active. */
	iwn_prph_write(sc, IWN4965_SCHED_QUEUE_STATUS(qid),
	    IWN4965_TXQ_STATUS_ACTIVE | IWN4965_TXQ_STATUS_AGGR_ENA |
	    iwn_tid2fifo[tid] << 1);
}

static void
iwn4965_ampdu_tx_stop(struct iwn_softc *sc, uint8_t tid, uint16_t ssn)
{
	int qid = 7 + tid;

	/* Stop TX scheduler while we're changing its configuration. */
	iwn_prph_write(sc, IWN4965_SCHED_QUEUE_STATUS(qid),
	    IWN4965_TXQ_STATUS_CHGACT);

	/* Set starting sequence number from the ADDBA request. */
	IWN_WRITE(sc, IWN_HBUS_TARG_WRPTR, qid << 8 | (ssn & 0xff));
	iwn_prph_write(sc, IWN4965_SCHED_QUEUE_RDPTR(qid), ssn);

	/* Disable interrupts for the queue. */
	iwn_prph_clrbits(sc, IWN4965_SCHED_INTR_MASK, 1 << qid);

	/* Mark the queue as inactive. */
	iwn_prph_write(sc, IWN4965_SCHED_QUEUE_STATUS(qid),
	    IWN4965_TXQ_STATUS_INACTIVE | iwn_tid2fifo[tid] << 1);
}

static void
iwn5000_ampdu_tx_start(struct iwn_softc *sc, struct ieee80211_node *ni,
    uint8_t tid, uint16_t ssn)
{
	struct iwn_node *wn = (void *)ni;
	int qid = 10 + tid;

	/* Stop TX scheduler while we're changing its configuration. */
	iwn_prph_write(sc, IWN5000_SCHED_QUEUE_STATUS(qid),
	    IWN5000_TXQ_STATUS_CHGACT);

	/* Assign RA/TID translation to the queue. */
	iwn_mem_write_2(sc, sc->sched_base + IWN5000_SCHED_TRANS_TBL(qid),
	    wn->id << 4 | tid);

	/* Enable chain-building mode for the queue. */
	iwn_prph_setbits(sc, IWN5000_SCHED_QCHAIN_SEL, 1 << qid);

	/* Enable aggregation for the queue. */
	iwn_prph_setbits(sc, IWN5000_SCHED_AGGR_SEL, 1 << qid);

	/* Set starting sequence number from the ADDBA request. */
	IWN_WRITE(sc, IWN_HBUS_TARG_WRPTR, qid << 8 | (ssn & 0xff));
	iwn_prph_write(sc, IWN5000_SCHED_QUEUE_RDPTR(qid), ssn);

	/* Set scheduler window size and frame limit. */
	iwn_mem_write(sc, sc->sched_base + IWN5000_SCHED_QUEUE_OFFSET(qid) + 4,
	    IWN_SCHED_LIMIT << 16 | IWN_SCHED_WINSZ);

	/* Enable interrupts for the queue. */
	iwn_prph_setbits(sc, IWN5000_SCHED_INTR_MASK, 1 << qid);

	/* Mark the queue as active. */
	iwn_prph_write(sc, IWN5000_SCHED_QUEUE_STATUS(qid),
	    IWN5000_TXQ_STATUS_ACTIVE | iwn_tid2fifo[tid]);
}

static void
iwn5000_ampdu_tx_stop(struct iwn_softc *sc, uint8_t tid, uint16_t ssn)
{
	int qid = 10 + tid;

	/* Stop TX scheduler while we're changing its configuration. */
	iwn_prph_write(sc, IWN5000_SCHED_QUEUE_STATUS(qid),
	    IWN5000_TXQ_STATUS_CHGACT);

	/* Disable aggregation for the queue. */
	iwn_prph_clrbits(sc, IWN5000_SCHED_AGGR_SEL, 1 << qid);

	/* Set starting sequence number from the ADDBA request. */
	IWN_WRITE(sc, IWN_HBUS_TARG_WRPTR, qid << 8 | (ssn & 0xff));
	iwn_prph_write(sc, IWN5000_SCHED_QUEUE_RDPTR(qid), ssn);

	/* Disable interrupts for the queue. */
	iwn_prph_clrbits(sc, IWN5000_SCHED_INTR_MASK, 1 << qid);

	/* Mark the queue as inactive. */
	iwn_prph_write(sc, IWN5000_SCHED_QUEUE_STATUS(qid),
	    IWN5000_TXQ_STATUS_INACTIVE | iwn_tid2fifo[tid]);
}
#endif

/*
 * Query calibration tables from the initialization firmware.  We do this
 * only once at first boot.  Called from a process context.
 */
static int
iwn5000_query_calibration(struct iwn_softc *sc)
{
	struct iwn5000_calib_config cmd;
	int error;

	memset(&cmd, 0, sizeof cmd);
	cmd.ucode.once.enable = 0xffffffff;
	cmd.ucode.once.start  = 0xffffffff;
	cmd.ucode.once.send   = 0xffffffff;
	cmd.ucode.flags       = 0xffffffff;
	DPRINTF(sc, IWN_DEBUG_CALIBRATE, "%s: sending calibration query\n",
	    __func__);
	error = iwn_cmd(sc, IWN5000_CMD_CALIB_CONFIG, &cmd, sizeof cmd, 0);
	if (error != 0)
		return error;

	/* Wait at most two seconds for calibration to complete. */
	if (!(sc->sc_flags & IWN_FLAG_CALIB_DONE))
		error = msleep(sc, &sc->sc_mtx, PCATCH, "iwninit", 2 * hz);
	return error;
}

/*
 * Send calibration results to the runtime firmware.  These results were
 * obtained on first boot from the initialization firmware.
 */
static int
iwn5000_send_calibration(struct iwn_softc *sc)
{
	int idx, error;

	for (idx = 0; idx < 5; idx++) {
		if (sc->calibcmd[idx].buf == NULL)
			continue;	/* No results available. */
		DPRINTF(sc, IWN_DEBUG_CALIBRATE,
		    "send calibration result idx=%d len=%d\n",
		    idx, sc->calibcmd[idx].len);
		error = iwn_cmd(sc, IWN_CMD_PHY_CALIB, sc->calibcmd[idx].buf,
		    sc->calibcmd[idx].len, 0);
		if (error != 0) {
			device_printf(sc->sc_dev,
			    "%s: could not send calibration result, error %d\n",
			    __func__, error);
			return error;
		}
	}
	return 0;
}

static int
iwn5000_send_wimax_coex(struct iwn_softc *sc)
{
	struct iwn5000_wimax_coex wimax;

#ifdef notyet
	if (sc->hw_type == IWN_HW_REV_TYPE_6050) {
		/* Enable WiMAX coexistence for combo adapters. */
		wimax.flags =
		    IWN_WIMAX_COEX_ASSOC_WA_UNMASK |
		    IWN_WIMAX_COEX_UNASSOC_WA_UNMASK |
		    IWN_WIMAX_COEX_STA_TABLE_VALID |
		    IWN_WIMAX_COEX_ENABLE;
		memcpy(wimax.events, iwn6050_wimax_events,
		    sizeof iwn6050_wimax_events);
	} else
#endif
	{
		/* Disable WiMAX coexistence. */
		wimax.flags = 0;
		memset(wimax.events, 0, sizeof wimax.events);
	}
	DPRINTF(sc, IWN_DEBUG_RESET, "%s: Configuring WiMAX coexistence\n",
	    __func__);
	return iwn_cmd(sc, IWN5000_CMD_WIMAX_COEX, &wimax, sizeof wimax, 0);
}

/*
 * This function is called after the runtime firmware notifies us of its
 * readiness (called in a process context.)
 */
static int
iwn4965_post_alive(struct iwn_softc *sc)
{
	int error, qid;

	if ((error = iwn_nic_lock(sc)) != 0)
		return error;

	/* Clear TX scheduler state in SRAM. */
	sc->sched_base = iwn_prph_read(sc, IWN_SCHED_SRAM_ADDR);
	iwn_mem_set_region_4(sc, sc->sched_base + IWN4965_SCHED_CTX_OFF, 0,
	    IWN4965_SCHED_CTX_LEN / sizeof (uint32_t));

	/* Set physical address of TX scheduler rings (1KB aligned.) */
	iwn_prph_write(sc, IWN4965_SCHED_DRAM_ADDR, sc->sched_dma.paddr >> 10);

	IWN_SETBITS(sc, IWN_FH_TX_CHICKEN, IWN_FH_TX_CHICKEN_SCHED_RETRY);

	/* Disable chain mode for all our 16 queues. */
	iwn_prph_write(sc, IWN4965_SCHED_QCHAIN_SEL, 0);

	for (qid = 0; qid < IWN4965_NTXQUEUES; qid++) {
		iwn_prph_write(sc, IWN4965_SCHED_QUEUE_RDPTR(qid), 0);
		IWN_WRITE(sc, IWN_HBUS_TARG_WRPTR, qid << 8 | 0);

		/* Set scheduler window size. */
		iwn_mem_write(sc, sc->sched_base +
		    IWN4965_SCHED_QUEUE_OFFSET(qid), IWN_SCHED_WINSZ);
		/* Set scheduler frame limit. */
		iwn_mem_write(sc, sc->sched_base +
		    IWN4965_SCHED_QUEUE_OFFSET(qid) + 4,
		    IWN_SCHED_LIMIT << 16);
	}

	/* Enable interrupts for all our 16 queues. */
	iwn_prph_write(sc, IWN4965_SCHED_INTR_MASK, 0xffff);
	/* Identify TX FIFO rings (0-7). */
	iwn_prph_write(sc, IWN4965_SCHED_TXFACT, 0xff);

	/* Mark TX rings (4 EDCA + cmd + 2 HCCA) as active. */
	for (qid = 0; qid < 7; qid++) {
		static uint8_t qid2fifo[] = { 3, 2, 1, 0, 4, 5, 6 };
		iwn_prph_write(sc, IWN4965_SCHED_QUEUE_STATUS(qid),
		    IWN4965_TXQ_STATUS_ACTIVE | qid2fifo[qid] << 1);
	}
	iwn_nic_unlock(sc);
	return 0;
}

/*
 * This function is called after the initialization or runtime firmware
 * notifies us of its readiness (called in a process context.)
 */
static int
iwn5000_post_alive(struct iwn_softc *sc)
{
	int error, qid;

	/* Switch to using ICT interrupt mode. */
	iwn5000_ict_reset(sc);

	error = iwn_nic_lock(sc);
	if (error != 0)
		return error;

	/* Clear TX scheduler state in SRAM. */
	sc->sched_base = iwn_prph_read(sc, IWN_SCHED_SRAM_ADDR);
	iwn_mem_set_region_4(sc, sc->sched_base + IWN5000_SCHED_CTX_OFF, 0,
	    IWN5000_SCHED_CTX_LEN / sizeof (uint32_t));

	/* Set physical address of TX scheduler rings (1KB aligned.) */
	iwn_prph_write(sc, IWN5000_SCHED_DRAM_ADDR, sc->sched_dma.paddr >> 10);

	IWN_SETBITS(sc, IWN_FH_TX_CHICKEN, IWN_FH_TX_CHICKEN_SCHED_RETRY);

	/* Enable chain mode for all queues, except command queue. */
	iwn_prph_write(sc, IWN5000_SCHED_QCHAIN_SEL, 0xfffef);
	iwn_prph_write(sc, IWN5000_SCHED_AGGR_SEL, 0);

	for (qid = 0; qid < IWN5000_NTXQUEUES; qid++) {
		iwn_prph_write(sc, IWN5000_SCHED_QUEUE_RDPTR(qid), 0);
		IWN_WRITE(sc, IWN_HBUS_TARG_WRPTR, qid << 8 | 0);

		iwn_mem_write(sc, sc->sched_base +
		    IWN5000_SCHED_QUEUE_OFFSET(qid), 0);
		/* Set scheduler window size and frame limit. */
		iwn_mem_write(sc, sc->sched_base +
		    IWN5000_SCHED_QUEUE_OFFSET(qid) + 4,
		    IWN_SCHED_LIMIT << 16 | IWN_SCHED_WINSZ);
	}

	/* Enable interrupts for all our 20 queues. */
	iwn_prph_write(sc, IWN5000_SCHED_INTR_MASK, 0xfffff);
	/* Identify TX FIFO rings (0-7). */
	iwn_prph_write(sc, IWN5000_SCHED_TXFACT, 0xff);

	/* Mark TX rings (4 EDCA + cmd + 2 HCCA) as active. */
	for (qid = 0; qid < 7; qid++) {
		static uint8_t qid2fifo[] = { 3, 2, 1, 0, 7, 5, 6 };
		iwn_prph_write(sc, IWN5000_SCHED_QUEUE_STATUS(qid),
		    IWN5000_TXQ_STATUS_ACTIVE | qid2fifo[qid]);
	}
	iwn_nic_unlock(sc);

	/* Configure WiMAX coexistence for combo adapters. */
	error = iwn5000_send_wimax_coex(sc);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not configure WiMAX coexistence, error %d\n",
		    __func__, error);
		return error;
	}
	if (sc->hw_type != IWN_HW_REV_TYPE_5150) {
		struct iwn5000_phy_calib_crystal cmd;

		/* Perform crystal calibration. */
		memset(&cmd, 0, sizeof cmd);
		cmd.code = IWN5000_PHY_CALIB_CRYSTAL;
		cmd.ngroups = 1;
		cmd.isvalid = 1;
		cmd.cap_pin[0] = le32toh(sc->eeprom_crystal) & 0xff;
		cmd.cap_pin[1] = (le32toh(sc->eeprom_crystal) >> 16) & 0xff;
		DPRINTF(sc, IWN_DEBUG_CALIBRATE,
		    "sending crystal calibration %d, %d\n",
		    cmd.cap_pin[0], cmd.cap_pin[1]);
		error = iwn_cmd(sc, IWN_CMD_PHY_CALIB, &cmd, sizeof cmd, 0);
		if (error != 0) {
			device_printf(sc->sc_dev,
			    "%s: crystal calibration failed, error %d\n",
			    __func__, error);
			return error;
		}
	}
	if (!(sc->sc_flags & IWN_FLAG_CALIB_DONE)) {
		/* Query calibration from the initialization firmware. */
		error = iwn5000_query_calibration(sc);
		if (error != 0) {
			device_printf(sc->sc_dev,
			    "%s: could not query calibration, error %d\n",
			    __func__, error);
			return error;
		}
		/*
		 * We have the calibration results now, reboot with the
		 * runtime firmware (call ourselves recursively!)
		 */
		iwn_hw_stop(sc);
		error = iwn_hw_init(sc);
	} else {
		/* Send calibration results to runtime firmware. */
		error = iwn5000_send_calibration(sc);
	}
	return error;
}

/*
 * The firmware boot code is small and is intended to be copied directly into
 * the NIC internal memory (no DMA transfer.)
 */
static int
iwn4965_load_bootcode(struct iwn_softc *sc, const uint8_t *ucode, int size)
{
	int error, ntries;

	size /= sizeof (uint32_t);

	error = iwn_nic_lock(sc);
	if (error != 0)
		return error;

	/* Copy microcode image into NIC memory. */
	iwn_prph_write_region_4(sc, IWN_BSM_SRAM_BASE,
	    (const uint32_t *)ucode, size);

	iwn_prph_write(sc, IWN_BSM_WR_MEM_SRC, 0);
	iwn_prph_write(sc, IWN_BSM_WR_MEM_DST, IWN_FW_TEXT_BASE);
	iwn_prph_write(sc, IWN_BSM_WR_DWCOUNT, size);

	/* Start boot load now. */
	iwn_prph_write(sc, IWN_BSM_WR_CTRL, IWN_BSM_WR_CTRL_START);

	/* Wait for transfer to complete. */
	for (ntries = 0; ntries < 1000; ntries++) {
		if (!(iwn_prph_read(sc, IWN_BSM_WR_CTRL) &
		    IWN_BSM_WR_CTRL_START))
			break;
		DELAY(10);
	}
	if (ntries == 1000) {
		device_printf(sc->sc_dev, "%s: could not load boot firmware\n",
		    __func__);
		iwn_nic_unlock(sc);
		return ETIMEDOUT;
	}

	/* Enable boot after power up. */
	iwn_prph_write(sc, IWN_BSM_WR_CTRL, IWN_BSM_WR_CTRL_START_EN);

	iwn_nic_unlock(sc);
	return 0;
}

static int
iwn4965_load_firmware(struct iwn_softc *sc)
{
	struct iwn_fw_info *fw = &sc->fw;
	struct iwn_dma_info *dma = &sc->fw_dma;
	int error;

	/* Copy initialization sections into pre-allocated DMA-safe memory. */
	memcpy(dma->vaddr, fw->init.data, fw->init.datasz);
	bus_dmamap_sync(sc->fw_dma.tag, dma->map, BUS_DMASYNC_PREWRITE);
	memcpy(dma->vaddr + IWN4965_FW_DATA_MAXSZ,
	    fw->init.text, fw->init.textsz);
	bus_dmamap_sync(sc->fw_dma.tag, dma->map, BUS_DMASYNC_PREWRITE);

	/* Tell adapter where to find initialization sections. */
	error = iwn_nic_lock(sc);
	if (error != 0)
		return error;
	iwn_prph_write(sc, IWN_BSM_DRAM_DATA_ADDR, dma->paddr >> 4);
	iwn_prph_write(sc, IWN_BSM_DRAM_DATA_SIZE, fw->init.datasz);
	iwn_prph_write(sc, IWN_BSM_DRAM_TEXT_ADDR,
	    (dma->paddr + IWN4965_FW_DATA_MAXSZ) >> 4);
	iwn_prph_write(sc, IWN_BSM_DRAM_TEXT_SIZE, fw->init.textsz);
	iwn_nic_unlock(sc);

	/* Load firmware boot code. */
	error = iwn4965_load_bootcode(sc, fw->boot.text, fw->boot.textsz);
	if (error != 0) {
		device_printf(sc->sc_dev, "%s: could not load boot firmware\n",
		    __func__);
		return error;
	}
	/* Now press "execute". */
	IWN_WRITE(sc, IWN_RESET, 0);

	/* Wait at most one second for first alive notification. */
	error = msleep(sc, &sc->sc_mtx, PCATCH, "iwninit", hz);
	if (error) {
		device_printf(sc->sc_dev,
		    "%s: timeout waiting for adapter to initialize, error %d\n",
		    __func__, error);
		return error;
	}

	/* Retrieve current temperature for initial TX power calibration. */
	sc->rawtemp = sc->ucode_info.temp[3].chan20MHz;
	sc->temp = iwn4965_get_temperature(sc);

	/* Copy runtime sections into pre-allocated DMA-safe memory. */
	memcpy(dma->vaddr, fw->main.data, fw->main.datasz);
	bus_dmamap_sync(sc->fw_dma.tag, dma->map, BUS_DMASYNC_PREWRITE);
	memcpy(dma->vaddr + IWN4965_FW_DATA_MAXSZ,
	    fw->main.text, fw->main.textsz);
	bus_dmamap_sync(sc->fw_dma.tag, dma->map, BUS_DMASYNC_PREWRITE);

	/* Tell adapter where to find runtime sections. */
	error = iwn_nic_lock(sc);
	if (error != 0)
		return error;

	iwn_prph_write(sc, IWN_BSM_DRAM_DATA_ADDR, dma->paddr >> 4);
	iwn_prph_write(sc, IWN_BSM_DRAM_DATA_SIZE, fw->main.datasz);
	iwn_prph_write(sc, IWN_BSM_DRAM_TEXT_ADDR,
	    (dma->paddr + IWN4965_FW_DATA_MAXSZ) >> 4);
	iwn_prph_write(sc, IWN_BSM_DRAM_TEXT_SIZE,
	    IWN_FW_UPDATED | fw->main.textsz);
	iwn_nic_unlock(sc);

	return 0;
}

static int
iwn5000_load_firmware_section(struct iwn_softc *sc, uint32_t dst,
    const uint8_t *section, int size)
{
	struct iwn_dma_info *dma = &sc->fw_dma;
	int error;

	/* Copy firmware section into pre-allocated DMA-safe memory. */
	memcpy(dma->vaddr, section, size);
	bus_dmamap_sync(sc->fw_dma.tag, dma->map, BUS_DMASYNC_PREWRITE);

	error = iwn_nic_lock(sc);
	if (error != 0)
		return error;

	IWN_WRITE(sc, IWN_FH_TX_CONFIG(IWN_SRVC_DMACHNL),
	    IWN_FH_TX_CONFIG_DMA_PAUSE);

	IWN_WRITE(sc, IWN_FH_SRAM_ADDR(IWN_SRVC_DMACHNL), dst);
	IWN_WRITE(sc, IWN_FH_TFBD_CTRL0(IWN_SRVC_DMACHNL),
	    IWN_LOADDR(dma->paddr));
	IWN_WRITE(sc, IWN_FH_TFBD_CTRL1(IWN_SRVC_DMACHNL),
	    IWN_HIADDR(dma->paddr) << 28 | size);
	IWN_WRITE(sc, IWN_FH_TXBUF_STATUS(IWN_SRVC_DMACHNL),
	    IWN_FH_TXBUF_STATUS_TBNUM(1) |
	    IWN_FH_TXBUF_STATUS_TBIDX(1) |
	    IWN_FH_TXBUF_STATUS_TFBD_VALID);

	/* Kick Flow Handler to start DMA transfer. */
	IWN_WRITE(sc, IWN_FH_TX_CONFIG(IWN_SRVC_DMACHNL),
	    IWN_FH_TX_CONFIG_DMA_ENA | IWN_FH_TX_CONFIG_CIRQ_HOST_ENDTFD);

	iwn_nic_unlock(sc);

	/* Wait at most five seconds for FH DMA transfer to complete. */
	return msleep(sc, &sc->sc_mtx, PCATCH, "iwninit", hz);
}

static int
iwn5000_load_firmware(struct iwn_softc *sc)
{
	struct iwn_fw_part *fw;
	int error;

	/* Load the initialization firmware on first boot only. */
	fw = (sc->sc_flags & IWN_FLAG_CALIB_DONE) ?
	    &sc->fw.main : &sc->fw.init;

	error = iwn5000_load_firmware_section(sc, IWN_FW_TEXT_BASE,
	    fw->text, fw->textsz);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not load firmware %s section, error %d\n",
		    __func__, ".text", error);
		return error;
	}
	error = iwn5000_load_firmware_section(sc, IWN_FW_DATA_BASE,
	    fw->data, fw->datasz);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not load firmware %s section, error %d\n",
		    __func__, ".data", error);
		return error;
	}

	/* Now press "execute". */
	IWN_WRITE(sc, IWN_RESET, 0);
	return 0;
}

static int
iwn_read_firmware(struct iwn_softc *sc)
{
	const struct iwn_hal *hal = sc->sc_hal;
	struct iwn_fw_info *fw = &sc->fw;
	const uint32_t *ptr;
	uint32_t rev;
	size_t size;

	IWN_UNLOCK(sc);

	/* Read firmware image from filesystem. */
	sc->fw_fp = firmware_get(sc->fwname);
	if (sc->fw_fp == NULL) {
		device_printf(sc->sc_dev,
		    "%s: could not load firmare image \"%s\"\n", __func__,
		    sc->fwname);
		IWN_LOCK(sc);
		return EINVAL;
	}
	IWN_LOCK(sc);

	size = sc->fw_fp->datasize;
	if (size < 28) {
		device_printf(sc->sc_dev,
		    "%s: truncated firmware header: %zu bytes\n",
		    __func__, size);
		return EINVAL;
	}

	/* Process firmware header. */
	ptr = (const uint32_t *)sc->fw_fp->data;
	rev = le32toh(*ptr++);
	/* Check firmware API version. */
	if (IWN_FW_API(rev) <= 1) {
		device_printf(sc->sc_dev,
		    "%s: bad firmware, need API version >=2\n", __func__);
		return EINVAL;
	}
	if (IWN_FW_API(rev) >= 3) {
		/* Skip build number (version 2 header). */
		size -= 4;
		ptr++;
	}
	fw->main.textsz = le32toh(*ptr++);
	fw->main.datasz = le32toh(*ptr++);
	fw->init.textsz = le32toh(*ptr++);
	fw->init.datasz = le32toh(*ptr++);
	fw->boot.textsz = le32toh(*ptr++);
	size -= 24;

	/* Sanity-check firmware header. */
	if (fw->main.textsz > hal->fw_text_maxsz ||
	    fw->main.datasz > hal->fw_data_maxsz ||
	    fw->init.textsz > hal->fw_text_maxsz ||
	    fw->init.datasz > hal->fw_data_maxsz ||
	    fw->boot.textsz > IWN_FW_BOOT_TEXT_MAXSZ ||
	    (fw->boot.textsz & 3) != 0) {
		device_printf(sc->sc_dev, "%s: invalid firmware header\n",
		    __func__);
		return EINVAL;
	}

	/* Check that all firmware sections fit. */
	if (fw->main.textsz + fw->main.datasz + fw->init.textsz +
	    fw->init.datasz + fw->boot.textsz > size) {
		device_printf(sc->sc_dev,
		    "%s: firmware file too short: %zu bytes\n",
		    __func__, size);
		return EINVAL;
	}

	/* Get pointers to firmware sections. */
	fw->main.text = (const uint8_t *)ptr;
	fw->main.data = fw->main.text + fw->main.textsz;
	fw->init.text = fw->main.data + fw->main.datasz;
	fw->init.data = fw->init.text + fw->init.textsz;
	fw->boot.text = fw->init.data + fw->init.datasz;

	return 0;
}

static int
iwn_clock_wait(struct iwn_softc *sc)
{
	int ntries;

	/* Set "initialization complete" bit. */
	IWN_SETBITS(sc, IWN_GP_CNTRL, IWN_GP_CNTRL_INIT_DONE);

	/* Wait for clock stabilization. */
	for (ntries = 0; ntries < 2500; ntries++) {
		if (IWN_READ(sc, IWN_GP_CNTRL) & IWN_GP_CNTRL_MAC_CLOCK_READY)
			return 0;
		DELAY(10);
	}
	device_printf(sc->sc_dev,
	    "%s: timeout waiting for clock stabilization\n", __func__);
	return ETIMEDOUT;
}

static int
iwn_apm_init(struct iwn_softc *sc)
{
	uint32_t tmp;
	int error;

	/* Disable L0s exit timer (NMI bug workaround.) */
	IWN_SETBITS(sc, IWN_GIO_CHICKEN, IWN_GIO_CHICKEN_DIS_L0S_TIMER);
	/* Don't wait for ICH L0s (ICH bug workaround.) */
	IWN_SETBITS(sc, IWN_GIO_CHICKEN, IWN_GIO_CHICKEN_L1A_NO_L0S_RX);

	/* Set FH wait threshold to max (HW bug under stress workaround.) */
	IWN_SETBITS(sc, IWN_DBG_HPET_MEM, 0xffff0000);

	/* Enable HAP INTA to move adapter from L1a to L0s. */
	IWN_SETBITS(sc, IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_HAP_WAKE_L1A);

	/* Retrieve PCIe Active State Power Management (ASPM). */
	tmp = pci_read_config(sc->sc_dev, sc->sc_cap_off + 0x10, 1);
	/* Workaround for HW instability in PCIe L0->L0s->L1 transition. */
	if (tmp & 0x02)	/* L1 Entry enabled. */
		IWN_SETBITS(sc, IWN_GIO, IWN_GIO_L0S_ENA);
	else
		IWN_CLRBITS(sc, IWN_GIO, IWN_GIO_L0S_ENA);

	if (sc->hw_type != IWN_HW_REV_TYPE_4965 &&
	    sc->hw_type != IWN_HW_REV_TYPE_6000 &&
	    sc->hw_type != IWN_HW_REV_TYPE_6050)
		IWN_SETBITS(sc, IWN_ANA_PLL, IWN_ANA_PLL_INIT);

	/* Wait for clock stabilization before accessing prph. */
	error = iwn_clock_wait(sc);
	if (error != 0)
		return error;

	error = iwn_nic_lock(sc);
	if (error != 0)
		return error;

	if (sc->hw_type == IWN_HW_REV_TYPE_4965) {
		/* Enable DMA and BSM (Bootstrap State Machine.) */
		iwn_prph_write(sc, IWN_APMG_CLK_EN,
		    IWN_APMG_CLK_CTRL_DMA_CLK_RQT |
		    IWN_APMG_CLK_CTRL_BSM_CLK_RQT);
	} else {
		/* Enable DMA. */
		iwn_prph_write(sc, IWN_APMG_CLK_EN,
		    IWN_APMG_CLK_CTRL_DMA_CLK_RQT);
	}
	DELAY(20);

	/* Disable L1-Active. */
	iwn_prph_setbits(sc, IWN_APMG_PCI_STT, IWN_APMG_PCI_STT_L1A_DIS);
	iwn_nic_unlock(sc);

	return 0;
}

static void
iwn_apm_stop_master(struct iwn_softc *sc)
{
	int ntries;

	/* Stop busmaster DMA activity. */
	IWN_SETBITS(sc, IWN_RESET, IWN_RESET_STOP_MASTER);
	for (ntries = 0; ntries < 100; ntries++) {
		if (IWN_READ(sc, IWN_RESET) & IWN_RESET_MASTER_DISABLED)
			return;
		DELAY(10);
	}
	device_printf(sc->sc_dev, "%s: timeout waiting for master\n",
	    __func__);
}

static void
iwn_apm_stop(struct iwn_softc *sc)
{
	iwn_apm_stop_master(sc);

	/* Reset the entire device. */
	IWN_SETBITS(sc, IWN_RESET, IWN_RESET_SW);
	DELAY(10);
	/* Clear "initialization complete" bit. */
	IWN_CLRBITS(sc, IWN_GP_CNTRL, IWN_GP_CNTRL_INIT_DONE);
}

static int
iwn4965_nic_config(struct iwn_softc *sc)
{
	if (IWN_RFCFG_TYPE(sc->rfcfg) == 1) {
		/*
		 * I don't believe this to be correct but this is what the
		 * vendor driver is doing. Probably the bits should not be
		 * shifted in IWN_RFCFG_*.
		 */
		IWN_SETBITS(sc, IWN_HW_IF_CONFIG,
		    IWN_RFCFG_TYPE(sc->rfcfg) |
		    IWN_RFCFG_STEP(sc->rfcfg) |
		    IWN_RFCFG_DASH(sc->rfcfg));
	}
	IWN_SETBITS(sc, IWN_HW_IF_CONFIG,
	    IWN_HW_IF_CONFIG_RADIO_SI | IWN_HW_IF_CONFIG_MAC_SI);
	return 0;
}

static int
iwn5000_nic_config(struct iwn_softc *sc)
{
	uint32_t tmp;
	int error;

	if (IWN_RFCFG_TYPE(sc->rfcfg) < 3) {
		IWN_SETBITS(sc, IWN_HW_IF_CONFIG,
		    IWN_RFCFG_TYPE(sc->rfcfg) |
		    IWN_RFCFG_STEP(sc->rfcfg) |
		    IWN_RFCFG_DASH(sc->rfcfg));
	}
	IWN_SETBITS(sc, IWN_HW_IF_CONFIG,
	    IWN_HW_IF_CONFIG_RADIO_SI | IWN_HW_IF_CONFIG_MAC_SI);

	error = iwn_nic_lock(sc);
	if (error != 0)
		return error;
	iwn_prph_setbits(sc, IWN_APMG_PS, IWN_APMG_PS_EARLY_PWROFF_DIS);

	if (sc->hw_type == IWN_HW_REV_TYPE_1000) {
		/*
		 * Select first Switching Voltage Regulator (1.32V) to
		 * solve a stability issue related to noisy DC2DC line
		 * in the silicon of 1000 Series.
		 */
		tmp = iwn_prph_read(sc, IWN_APMG_DIGITAL_SVR);
		tmp &= ~IWN_APMG_DIGITAL_SVR_VOLTAGE_MASK;
		tmp |= IWN_APMG_DIGITAL_SVR_VOLTAGE_1_32;
		iwn_prph_write(sc, IWN_APMG_DIGITAL_SVR, tmp);
	}
	iwn_nic_unlock(sc);

	if (sc->sc_flags & IWN_FLAG_INTERNAL_PA) {
		/* Use internal power amplifier only. */
		IWN_WRITE(sc, IWN_GP_DRIVER, IWN_GP_DRIVER_RADIO_2X2_IPA);
	}
	 if (sc->hw_type == IWN_HW_REV_TYPE_6050 && sc->calib_ver >= 6) {
		 /* Indicate that ROM calibration version is >=6. */
		 IWN_SETBITS(sc, IWN_GP_DRIVER, IWN_GP_DRIVER_CALIB_VER6);
	}
	return 0;
}

/*
 * Take NIC ownership over Intel Active Management Technology (AMT).
 */
static int
iwn_hw_prepare(struct iwn_softc *sc)
{
	int ntries;

	/* Check if hardware is ready. */
	IWN_SETBITS(sc, IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_NIC_READY);
	for (ntries = 0; ntries < 5; ntries++) {
		if (IWN_READ(sc, IWN_HW_IF_CONFIG) &
		    IWN_HW_IF_CONFIG_NIC_READY)
			return 0;
		DELAY(10);
	}

	/* Hardware not ready, force into ready state. */
	IWN_SETBITS(sc, IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_PREPARE);
	for (ntries = 0; ntries < 15000; ntries++) {
		if (!(IWN_READ(sc, IWN_HW_IF_CONFIG) &
		    IWN_HW_IF_CONFIG_PREPARE_DONE))
			break;
		DELAY(10);
	}
	if (ntries == 15000)
		return ETIMEDOUT;

	/* Hardware should be ready now. */
	IWN_SETBITS(sc, IWN_HW_IF_CONFIG, IWN_HW_IF_CONFIG_NIC_READY);
	for (ntries = 0; ntries < 5; ntries++) {
		if (IWN_READ(sc, IWN_HW_IF_CONFIG) &
		    IWN_HW_IF_CONFIG_NIC_READY)
			return 0;
		DELAY(10);
	}
	return ETIMEDOUT;
}

static int
iwn_hw_init(struct iwn_softc *sc)
{
	const struct iwn_hal *hal = sc->sc_hal;
	int error, chnl, qid;

	/* Clear pending interrupts. */
	IWN_WRITE(sc, IWN_INT, 0xffffffff);

	error = iwn_apm_init(sc);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not power ON adapter, error %d\n",
		    __func__, error);
		return error;
	}

	/* Select VMAIN power source. */
	error = iwn_nic_lock(sc);
	if (error != 0)
		return error;
	iwn_prph_clrbits(sc, IWN_APMG_PS, IWN_APMG_PS_PWR_SRC_MASK);
	iwn_nic_unlock(sc);

	/* Perform adapter-specific initialization. */
	error = hal->nic_config(sc);
	if (error != 0)
		return error;

	/* Initialize RX ring. */
	error = iwn_nic_lock(sc);
	if (error != 0)
		return error;
	IWN_WRITE(sc, IWN_FH_RX_CONFIG, 0);
	IWN_WRITE(sc, IWN_FH_RX_WPTR, 0);
	/* Set physical address of RX ring (256-byte aligned.) */
	IWN_WRITE(sc, IWN_FH_RX_BASE, sc->rxq.desc_dma.paddr >> 8);
	/* Set physical address of RX status (16-byte aligned.) */
	IWN_WRITE(sc, IWN_FH_STATUS_WPTR, sc->rxq.stat_dma.paddr >> 4);
	/* Enable RX. */
	IWN_WRITE(sc, IWN_FH_RX_CONFIG,
	    IWN_FH_RX_CONFIG_ENA           |
	    IWN_FH_RX_CONFIG_IGN_RXF_EMPTY |	/* HW bug workaround */
	    IWN_FH_RX_CONFIG_IRQ_DST_HOST  |
	    IWN_FH_RX_CONFIG_SINGLE_FRAME  |
	    IWN_FH_RX_CONFIG_RB_TIMEOUT(0) |
	    IWN_FH_RX_CONFIG_NRBD(IWN_RX_RING_COUNT_LOG));
	iwn_nic_unlock(sc);
	IWN_WRITE(sc, IWN_FH_RX_WPTR, (IWN_RX_RING_COUNT - 1) & ~7);

	error = iwn_nic_lock(sc);
	if (error != 0)
		return error;

	/* Initialize TX scheduler. */
	iwn_prph_write(sc, hal->sched_txfact_addr, 0);

	/* Set physical address of "keep warm" page (16-byte aligned.) */
	IWN_WRITE(sc, IWN_FH_KW_ADDR, sc->kw_dma.paddr >> 4);

	/* Initialize TX rings. */
	for (qid = 0; qid < hal->ntxqs; qid++) {
		struct iwn_tx_ring *txq = &sc->txq[qid];

		/* Set physical address of TX ring (256-byte aligned.) */
		IWN_WRITE(sc, IWN_FH_CBBC_QUEUE(qid),
		    txq->desc_dma.paddr >> 8);
	}
	iwn_nic_unlock(sc);

	/* Enable DMA channels. */
	for (chnl = 0; chnl < hal->ndmachnls; chnl++) {
		IWN_WRITE(sc, IWN_FH_TX_CONFIG(chnl),
		    IWN_FH_TX_CONFIG_DMA_ENA |
		    IWN_FH_TX_CONFIG_DMA_CREDIT_ENA);
	}

	/* Clear "radio off" and "commands blocked" bits. */
	IWN_WRITE(sc, IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_RFKILL);
	IWN_WRITE(sc, IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_CMD_BLOCKED);

	/* Clear pending interrupts. */
	IWN_WRITE(sc, IWN_INT, 0xffffffff);
	/* Enable interrupt coalescing. */
	IWN_WRITE(sc, IWN_INT_COALESCING, 512 / 8);
	/* Enable interrupts. */
	IWN_WRITE(sc, IWN_INT_MASK, sc->int_mask);

	/* _Really_ make sure "radio off" bit is cleared! */
	IWN_WRITE(sc, IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_RFKILL);
	IWN_WRITE(sc, IWN_UCODE_GP1_CLR, IWN_UCODE_GP1_RFKILL);

	error = hal->load_firmware(sc);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not load firmware, error %d\n",
		    __func__, error);
		return error;
	}
	/* Wait at most one second for firmware alive notification. */
	error = msleep(sc, &sc->sc_mtx, PCATCH, "iwninit", hz);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: timeout waiting for adapter to initialize, error %d\n",
		    __func__, error);
		return error;
	}
	/* Do post-firmware initialization. */
	return hal->post_alive(sc);
}

static void
iwn_hw_stop(struct iwn_softc *sc)
{
	const struct iwn_hal *hal = sc->sc_hal;
	uint32_t tmp;
	int chnl, qid, ntries;

	IWN_WRITE(sc, IWN_RESET, IWN_RESET_NEVO);

	/* Disable interrupts. */
	IWN_WRITE(sc, IWN_INT_MASK, 0);
	IWN_WRITE(sc, IWN_INT, 0xffffffff);
	IWN_WRITE(sc, IWN_FH_INT, 0xffffffff);
	sc->sc_flags &= ~IWN_FLAG_USE_ICT;

	/* Make sure we no longer hold the NIC lock. */
	iwn_nic_unlock(sc);

	/* Stop TX scheduler. */
	iwn_prph_write(sc, hal->sched_txfact_addr, 0);

	/* Stop all DMA channels. */
	if (iwn_nic_lock(sc) == 0) {
		for (chnl = 0; chnl < hal->ndmachnls; chnl++) {
			IWN_WRITE(sc, IWN_FH_TX_CONFIG(chnl), 0);
			for (ntries = 0; ntries < 200; ntries++) {
				tmp = IWN_READ(sc, IWN_FH_TX_STATUS);
				if ((tmp & IWN_FH_TX_STATUS_IDLE(chnl)) ==
				    IWN_FH_TX_STATUS_IDLE(chnl))
					break;
				DELAY(10);
			}
		}
		iwn_nic_unlock(sc);
	}

	/* Stop RX ring. */
	iwn_reset_rx_ring(sc, &sc->rxq);

	/* Reset all TX rings. */
	for (qid = 0; qid < hal->ntxqs; qid++)
		iwn_reset_tx_ring(sc, &sc->txq[qid]);

	if (iwn_nic_lock(sc) == 0) {
		iwn_prph_write(sc, IWN_APMG_CLK_DIS,
		    IWN_APMG_CLK_CTRL_DMA_CLK_RQT);
		iwn_nic_unlock(sc);
	}
	DELAY(5);

	/* Power OFF adapter. */
	iwn_apm_stop(sc);
}

static void
iwn_init_locked(struct iwn_softc *sc)
{
	struct ifnet *ifp = sc->sc_ifp;
	int error;

	IWN_LOCK_ASSERT(sc);

	error = iwn_hw_prepare(sc);
	if (error != 0) {
		device_printf(sc->sc_dev, "%s: hardware not ready, eror %d\n",
		    __func__, error);
		goto fail;
	}

	/* Initialize interrupt mask to default value. */
	sc->int_mask = IWN_INT_MASK_DEF;
	sc->sc_flags &= ~IWN_FLAG_USE_ICT;

	/* Check that the radio is not disabled by hardware switch. */
	if (!(IWN_READ(sc, IWN_GP_CNTRL) & IWN_GP_CNTRL_RFKILL)) {
		device_printf(sc->sc_dev,
		    "radio is disabled by hardware switch\n");

		/* Enable interrupts to get RF toggle notifications. */
		IWN_WRITE(sc, IWN_INT, 0xffffffff);
		IWN_WRITE(sc, IWN_INT_MASK, sc->int_mask);
		return;
	}

	/* Read firmware images from the filesystem. */
	error = iwn_read_firmware(sc);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not read firmware, error %d\n",
		    __func__, error);
		goto fail;
	}

	/* Initialize hardware and upload firmware. */
	error = iwn_hw_init(sc);
	firmware_put(sc->fw_fp, FIRMWARE_UNLOAD);
	sc->fw_fp = NULL;
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not initialize hardware, error %d\n",
		    __func__, error);
		goto fail;
	}

	/* Configure adapter now that it is ready. */
	error = iwn_config(sc);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "%s: could not configure device, error %d\n",
		    __func__, error);
		goto fail;
	}

	ifp->if_drv_flags &= ~IFF_DRV_OACTIVE;
	ifp->if_drv_flags |= IFF_DRV_RUNNING;

	return;

fail:
	iwn_stop_locked(sc);
}

static void
iwn_init(void *arg)
{
	struct iwn_softc *sc = arg;
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;

	IWN_LOCK(sc);
	iwn_init_locked(sc);
	IWN_UNLOCK(sc);

	if (ifp->if_drv_flags & IFF_DRV_RUNNING)
		ieee80211_start_all(ic);
}

static void
iwn_stop_locked(struct iwn_softc *sc)
{
	struct ifnet *ifp = sc->sc_ifp;

	IWN_LOCK_ASSERT(sc);

	sc->sc_tx_timer = 0;
	callout_stop(&sc->sc_timer_to);
	ifp->if_drv_flags &= ~(IFF_DRV_RUNNING | IFF_DRV_OACTIVE);

	/* Power OFF hardware. */
	iwn_hw_stop(sc);
}

static void
iwn_stop(struct iwn_softc *sc)
{
	IWN_LOCK(sc);
	iwn_stop_locked(sc);
	IWN_UNLOCK(sc);
}

/*
 * Callback from net80211 to start a scan.
 */
static void
iwn_scan_start(struct ieee80211com *ic)
{
	struct ifnet *ifp = ic->ic_ifp;
	struct iwn_softc *sc = ifp->if_softc;

	IWN_LOCK(sc);
	/* make the link LED blink while we're scanning */
	iwn_set_led(sc, IWN_LED_LINK, 20, 2);
	IWN_UNLOCK(sc);
}

/*
 * Callback from net80211 to terminate a scan.
 */
static void
iwn_scan_end(struct ieee80211com *ic)
{
	struct ifnet *ifp = ic->ic_ifp;
	struct iwn_softc *sc = ifp->if_softc;
	struct ieee80211vap *vap = TAILQ_FIRST(&ic->ic_vaps);

	IWN_LOCK(sc);
	if (vap->iv_state == IEEE80211_S_RUN) {
		/* Set link LED to ON status if we are associated */
		iwn_set_led(sc, IWN_LED_LINK, 0, 1);
	}
	IWN_UNLOCK(sc);
}

/*
 * Callback from net80211 to force a channel change.
 */
static void
iwn_set_channel(struct ieee80211com *ic)
{
	const struct ieee80211_channel *c = ic->ic_curchan;
	struct ifnet *ifp = ic->ic_ifp;
	struct iwn_softc *sc = ifp->if_softc;

	IWN_LOCK(sc);
	sc->sc_rxtap.wr_chan_freq = htole16(c->ic_freq);
	sc->sc_rxtap.wr_chan_flags = htole16(c->ic_flags);
	sc->sc_txtap.wt_chan_freq = htole16(c->ic_freq);
	sc->sc_txtap.wt_chan_flags = htole16(c->ic_flags);
	IWN_UNLOCK(sc);
}

/*
 * Callback from net80211 to start scanning of the current channel.
 */
static void
iwn_scan_curchan(struct ieee80211_scan_state *ss, unsigned long maxdwell)
{
	struct ieee80211vap *vap = ss->ss_vap;
	struct iwn_softc *sc = vap->iv_ic->ic_ifp->if_softc;
	int error;

	IWN_LOCK(sc);
	error = iwn_scan(sc);
	IWN_UNLOCK(sc);
	if (error != 0)
		ieee80211_cancel_scan(vap);
}

/*
 * Callback from net80211 to handle the minimum dwell time being met.
 * The intent is to terminate the scan but we just let the firmware
 * notify us when it's finished as we have no safe way to abort it.
 */
static void
iwn_scan_mindwell(struct ieee80211_scan_state *ss)
{
	/* NB: don't try to abort scan; wait for firmware to finish */
}

static struct iwn_eeprom_chan *
iwn_find_eeprom_channel(struct iwn_softc *sc, struct ieee80211_channel *c)
{
	int i, j;

	for (j = 0; j < 7; j++) {
		for (i = 0; i < iwn_bands[j].nchan; i++) {
			if (iwn_bands[j].chan[i] == c->ic_ieee)
				return &sc->eeprom_channels[j][i];
		}
	}

	return NULL;
}

/*
 * Enforce flags read from EEPROM.
 */
static int
iwn_setregdomain(struct ieee80211com *ic, struct ieee80211_regdomain *rd,
    int nchan, struct ieee80211_channel chans[])
{
	struct iwn_softc *sc = ic->ic_ifp->if_softc;
	int i;

	for (i = 0; i < nchan; i++) {
		struct ieee80211_channel *c = &chans[i];
		struct iwn_eeprom_chan *channel;

		channel = iwn_find_eeprom_channel(sc, c);
		if (channel == NULL) {
			if_printf(ic->ic_ifp,
			    "%s: invalid channel %u freq %u/0x%x\n",
			    __func__, c->ic_ieee, c->ic_freq, c->ic_flags);
			return EINVAL;
		}
		c->ic_flags |= iwn_eeprom_channel_flags(channel);
	}

	return 0;
}

static void
iwn_hw_reset(void *arg0, int pending)
{
	struct iwn_softc *sc = arg0;
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;

	iwn_stop(sc);
	iwn_init(sc);
	ieee80211_notify_radio(ic, 1);
}

static void
iwn_radio_on(void *arg0, int pending)
{
	struct iwn_softc *sc = arg0;
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	struct ieee80211vap *vap = TAILQ_FIRST(&ic->ic_vaps);

	if (vap != NULL) {
		iwn_init(sc);
		ieee80211_init(vap);
	}
}

static void
iwn_radio_off(void *arg0, int pending)
{
	struct iwn_softc *sc = arg0;
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	struct ieee80211vap *vap = TAILQ_FIRST(&ic->ic_vaps);

	iwn_stop(sc);
	if (vap != NULL)
		ieee80211_stop(vap);

	/* Enable interrupts to get RF toggle notification. */
	IWN_LOCK(sc);
	IWN_WRITE(sc, IWN_INT, 0xffffffff);
	IWN_WRITE(sc, IWN_INT_MASK, sc->int_mask);
	IWN_UNLOCK(sc);
}

static void
iwn_sysctlattach(struct iwn_softc *sc)
{
	struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(sc->sc_dev);
	struct sysctl_oid *tree = device_get_sysctl_tree(sc->sc_dev);

#ifdef IWN_DEBUG
	sc->sc_debug = 0;
	SYSCTL_ADD_INT(ctx, SYSCTL_CHILDREN(tree), OID_AUTO,
	    "debug", CTLFLAG_RW, &sc->sc_debug, 0, "control debugging printfs");
#endif
}

static int
iwn_shutdown(device_t dev)
{
	struct iwn_softc *sc = device_get_softc(dev);

	iwn_stop(sc);
	return 0;
}

static int
iwn_suspend(device_t dev)
{
	struct iwn_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	struct ieee80211vap *vap = TAILQ_FIRST(&ic->ic_vaps);

	iwn_stop(sc);
	if (vap != NULL)
		ieee80211_stop(vap);
	return 0;
}

static int
iwn_resume(device_t dev)
{
	struct iwn_softc *sc = device_get_softc(dev);
	struct ifnet *ifp = sc->sc_ifp;
	struct ieee80211com *ic = ifp->if_l2com;
	struct ieee80211vap *vap = TAILQ_FIRST(&ic->ic_vaps);

	/* Clear device-specific "PCI retry timeout" register (41h). */
	pci_write_config(dev, 0x41, 0, 1);

	if (ifp->if_flags & IFF_UP) {
		iwn_init(sc);
		if (vap != NULL)
			ieee80211_init(vap);
		if (ifp->if_drv_flags & IFF_DRV_RUNNING)
			iwn_start(ifp);
	}
	return 0;
}

#ifdef IWN_DEBUG
static const char *
iwn_intr_str(uint8_t cmd)
{
	switch (cmd) {
	/* Notifications */
	case IWN_UC_READY:		return "UC_READY";
	case IWN_ADD_NODE_DONE:		return "ADD_NODE_DONE";
	case IWN_TX_DONE:		return "TX_DONE";
	case IWN_START_SCAN:		return "START_SCAN";
	case IWN_STOP_SCAN:		return "STOP_SCAN";
	case IWN_RX_STATISTICS:		return "RX_STATS";
	case IWN_BEACON_STATISTICS:	return "BEACON_STATS";
	case IWN_STATE_CHANGED:		return "STATE_CHANGED";
	case IWN_BEACON_MISSED:		return "BEACON_MISSED";
	case IWN_RX_PHY:		return "RX_PHY";
	case IWN_MPDU_RX_DONE:		return "MPDU_RX_DONE";
	case IWN_RX_DONE:		return "RX_DONE";

	/* Command Notifications */
	case IWN_CMD_RXON:		return "IWN_CMD_RXON";
	case IWN_CMD_RXON_ASSOC:	return "IWN_CMD_RXON_ASSOC";
	case IWN_CMD_EDCA_PARAMS:	return "IWN_CMD_EDCA_PARAMS";
	case IWN_CMD_TIMING:		return "IWN_CMD_TIMING";
	case IWN_CMD_LINK_QUALITY:	return "IWN_CMD_LINK_QUALITY";
	case IWN_CMD_SET_LED:		return "IWN_CMD_SET_LED";
	case IWN5000_CMD_WIMAX_COEX:	return "IWN5000_CMD_WIMAX_COEX";
	case IWN5000_CMD_CALIB_CONFIG:	return "IWN5000_CMD_CALIB_CONFIG";
	case IWN5000_CMD_CALIB_RESULT:	return "IWN5000_CMD_CALIB_RESULT";
	case IWN5000_CMD_CALIB_COMPLETE: return "IWN5000_CMD_CALIB_COMPLETE";
	case IWN_CMD_SET_POWER_MODE:	return "IWN_CMD_SET_POWER_MODE";
	case IWN_CMD_SCAN:		return "IWN_CMD_SCAN";
	case IWN_CMD_SCAN_RESULTS:	return "IWN_CMD_SCAN_RESULTS";
	case IWN_CMD_TXPOWER:		return "IWN_CMD_TXPOWER";
	case IWN_CMD_TXPOWER_DBM:	return "IWN_CMD_TXPOWER_DBM";
	case IWN5000_CMD_TX_ANT_CONFIG:	return "IWN5000_CMD_TX_ANT_CONFIG";
	case IWN_CMD_BT_COEX:		return "IWN_CMD_BT_COEX";
	case IWN_CMD_SET_CRITICAL_TEMP:	return "IWN_CMD_SET_CRITICAL_TEMP";
	case IWN_CMD_SET_SENSITIVITY:	return "IWN_CMD_SET_SENSITIVITY";
	case IWN_CMD_PHY_CALIB:		return "IWN_CMD_PHY_CALIB";
	}
	return "UNKNOWN INTR NOTIF/CMD";
}
#endif /* IWN_DEBUG */

static device_method_t iwn_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		iwn_probe),
	DEVMETHOD(device_attach,	iwn_attach),
	DEVMETHOD(device_detach,	iwn_detach),
	DEVMETHOD(device_shutdown,	iwn_shutdown),
	DEVMETHOD(device_suspend,	iwn_suspend),
	DEVMETHOD(device_resume,	iwn_resume),
	{ 0, 0 }
};

static driver_t iwn_driver = {
	"iwn",
	iwn_methods,
	sizeof (struct iwn_softc)
};
static devclass_t iwn_devclass;

DRIVER_MODULE(iwn, pci, iwn_driver, iwn_devclass, 0, 0);
MODULE_DEPEND(iwn, pci, 1, 1, 1);
MODULE_DEPEND(iwn, firmware, 1, 1, 1);
MODULE_DEPEND(iwn, wlan, 1, 1, 1);
MODULE_DEPEND(iwn, wlan_amrr, 1, 1, 1);
