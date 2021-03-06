#ifndef	__IF_IWA_TRANS_H__
#define	__IF_IWA_TRANS_H__

/*
 * This file defines the bus-facing primitives and functions.
 * It's very PCIe oriented at the moment, but there's apparently
 * some Intel SDIO wifi parts that will work with the same
 * driver code as the other 7xxx/8xxx chips.
 *
 * So for now let's try to keep as much of the bus specific
 * and transmit/receive/command ring management code here.
 */

/*
 * The linux iwlwifi driver uses a 16-bit little endian
 * field called 'sequence' in the iwl_cmd_header struct.
 *
 * Bits 0:14 are user-settable and get copied from a
 * driver-issued command into the response - this is how
 * the driver can map RX'ed command responses to their
 * initial TX'ed commands.
 *
 * Bit 15 is set if the response was firmware generated,
 * rather than a response to a driver-sent command.
 *
 * These macros translate the host-order bits into
 * the field (and vice versa.)  The caller should
 * do the le16 translation.
 */

#define	IWA_SEQ_TO_IDX(v)	((v) & 0xff)
#define	IWA_SEQ_TO_QID(v)	(((v) >> 8) & 0x1f)
#define	IWA_SEQ_TO_UCODE_RX(v)	((v) & 0x8000)

/* And now, assembling the idx/qid entry */
#define	IWA_IDX_QID_TO_SEQ(s, q)	(((s) & 0xff) | (((q) & 0x1f) << 8))

struct iwa_dma_info {
        bus_dma_tag_t           tag;
        bus_dmamap_t            map;
        bus_dma_segment_t       seg;
        bus_addr_t              paddr;
        caddr_t                 vaddr;
        bus_size_t              size;
};

#define IWM_ICT_SIZE            4096
#define IWM_ICT_COUNT           (IWM_ICT_SIZE / sizeof (uint32_t))
#define IWM_ICT_PADDR_SHIFT     12

#define IWA_TX_RING_COUNT       256
#define IWA_TX_RING_LOMARK      192
#define IWA_TX_RING_HIMARK      224

struct iwa_tx_data {
        bus_dmamap_t    map;
        bus_addr_t      cmd_paddr;
        bus_addr_t      scratch_paddr;
        struct mbuf     *m;
        struct iwa_node *in;
        bool done;
};

struct iwa_tx_ring {
        struct iwa_dma_info     desc_dma;
        struct iwa_dma_info     cmd_dma;
        struct iwl_tfd          *desc;
        struct iwl_device_cmd   *cmd;
        struct iwa_tx_data      data[IWA_TX_RING_COUNT];
	bus_dma_tag_t           data_dmat;
        int                     qid;
        int                     queued;
        int                     cur;
};

/*
 * This represents the link "back" from the sending
 * command slot entry to the hcmd that originated
 * it.
 *
 * For async TX commands, the slot entry will be NULL.
 * For sync TX commands that don't want the response,
 *   the slot entry will be NULL.
 * For sync TX commands that want a response, the
 *  slot entry will be not-NULL.
 *
 * // if 0
 * I'm cheating by using a void pointer here so
 * I don't have to worry about getting types right.
 * But it's intended to be a iwl_host_cmd pointer.
 * // endif
 *
 * Note!  This is where things get murky.
 * This is intended to be protected by the IWA lock.
 * That way if the caller errors out before the
 * interrupt routine fires, the caller can remove
 * the ring slot pointer so the completion path
 * won't notify the now-stale iwl_host_cmd pointer.
 */

struct iwa_tx_ring_meta {
	struct iwl_host_cmd *meta[IWA_TX_RING_COUNT];
};

#define IWA_RX_RING_COUNT       256
#define IWA_RBUF_COUNT          (IWA_RX_RING_COUNT + 32)
/* Linux driver optionally uses 8k buffer */
#define IWA_RBUF_SIZE           4096

struct iwa_softc;
struct iwa_rbuf {
        struct iwa_softc        *sc;
        void                    *vaddr;
        bus_addr_t              paddr;
};

struct iwa_rx_data {
        struct mbuf     *m;
        bus_dmamap_t    map;
        int             wantresp;
};

struct iwa_rx_ring {
        struct iwa_dma_info     desc_dma;
        struct iwa_dma_info     stat_dma;
        struct iwa_dma_info     buf_dma;
        uint32_t                *desc;
        struct iwl_rb_status    *stat;
        struct iwa_rx_data      data[IWA_RX_RING_COUNT];
	bus_dma_tag_t           data_dmat;
        int                     cur;
};

/* Bus method */
extern	void iwa_dma_map_addr(void *arg, bus_dma_segment_t *segs,
	    int nsegs, int error);

/*
 * External facing functions - should be trans ops methods!
 */
extern	void iwa_set_pwr(struct iwa_softc *sc);
extern	int iwa_prepare_card_hw(struct iwa_softc *sc);
extern	int iwa_start_hw(struct iwa_softc *sc);
extern	void iwa_stop_device(struct iwa_softc *sc);
extern	bool iwa_check_rfkill(struct iwa_softc *sc);
extern	void iwa_enable_interrupts(struct iwa_softc *sc);
extern	void iwa_restore_interrupts(struct iwa_softc *sc);
extern	void iwa_disable_interrupts(struct iwa_softc *sc);
extern	bool iwa_grab_nic_access(struct iwa_softc *sc);
extern	void iwa_release_nic_access(struct iwa_softc *sc);
extern	int iwa_nic_rx_init(struct iwa_softc *sc);
extern	int iwa_nic_tx_init(struct iwa_softc *sc);
extern	int iwa_nic_init(struct iwa_softc *sc);
extern	int iwa_post_alive(struct iwa_softc *sc);

/* bus things that should go into if_iwareg.h */
extern	void iwa_set_bit(struct iwa_softc *sc, int reg, uint32_t bit);
extern	void iwa_clear_bit(struct iwa_softc *sc, int reg, uint32_t bit);
extern	void iwa_set_bits_mask(struct iwa_softc *sc, int reg, uint32_t mask,
	    uint32_t bits);
extern	bool iwa_poll_bit(struct iwa_softc *sc, int reg,
	    uint32_t bits, uint32_t mask, int timo);

extern	int iwa_alloc_fwmem(struct iwa_softc *sc);
extern	void iwa_free_fwmem(struct iwa_softc *sc);
extern	int iwa_alloc_sched(struct iwa_softc *sc);
extern	void iwa_free_sched(struct iwa_softc *sc);
extern	int iwa_alloc_kw(struct iwa_softc *sc);
extern	void iwa_free_kw(struct iwa_softc *sc);
extern	int iwa_alloc_ict(struct iwa_softc *sc);
extern	void iwa_free_ict(struct iwa_softc *sc);
extern	int iwa_rx_addbuf(struct iwa_softc *sc, struct iwa_rx_ring *ring,
	    size_t mbuf_size, int idx);
extern	int iwa_alloc_rx_ring(struct iwa_softc *sc, struct iwa_rx_ring *ring);
extern	void iwa_reset_rx_ring(struct iwa_softc *sc, struct iwa_rx_ring *ring);
extern	void iwa_free_rx_ring(struct iwa_softc *sc, struct iwa_rx_ring *ring);
extern	int iwa_alloc_tx_ring(struct iwa_softc *sc, struct iwa_tx_ring *ring,
	    int qid);
extern	void iwa_reset_tx_ring(struct iwa_softc *sc, struct iwa_tx_ring *ring);
extern	void iwa_free_tx_ring(struct iwa_softc *sc, struct iwa_tx_ring *ring);

#endif	/* __IF_IWA_TRANS_H__ */
