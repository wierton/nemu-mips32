#if CONFIG_ETHERLITE
#  include <arpa/inet.h>
#  include <net/if.h>
#  include <netinet/ether.h>
#  include <netpacket/packet.h>
#  include <sys/ioctl.h>
#  include <sys/socket.h>
#  include <sys/types.h>

#  include "debug.h"
#  include "device.h"
#  include "utils.h"

static pcap_handler pcap;

// emaclite
#  define MAC_ADDR CONFIG_ETHERLITE_BASE
#  define MAC_SIZE 0x10000

/* emaclite protocol */
#  define ENET_ADDR_LENGTH 6
#  define ETH_FCS_LEN 4 /* Octets in the FCS */

/* Xmit complete */
#  define XEL_TSR_XMIT_BUSY_MASK 0x00000001UL
/* Xmit interrupt enable bit */
#  define XEL_TSR_XMIT_IE_MASK 0x00000008UL
/* Program the MAC address */
#  define XEL_TSR_PROGRAM_MASK 0x00000002UL
/* define for programming the MAC address into the EMAC Lite
 */
#  define XEL_TSR_PROG_MAC_ADDR (XEL_TSR_XMIT_BUSY_MASK | XEL_TSR_PROGRAM_MASK)

/* Transmit packet length upper byte */
#  define XEL_TPLR_LENGTH_MASK_HI 0x0000FF00UL
/* Transmit packet length lower byte */
#  define XEL_TPLR_LENGTH_MASK_LO 0x000000FFUL

/* Recv complete */
#  define XEL_RSR_RECV_DONE_MASK 0x00000001UL
/* Recv interrupt enable bit */
#  define XEL_RSR_RECV_IE_MASK 0x00000008UL

/* MDIO Address Register Bit Masks */
#  define XEL_MDIOADDR_REGADR_MASK 0x0000001F /* Register Address */
#  define XEL_MDIOADDR_PHYADR_MASK 0x000003E0 /* PHY Address */
#  define XEL_MDIOADDR_PHYADR_SHIFT 5
#  define XEL_MDIOADDR_OP_MASK 0x00000400 /* RD/WR Operation */

/* MDIO Write Data Register Bit Masks */
#  define XEL_MDIOWR_WRDATA_MASK 0x0000FFFF /* Data to be Written */

/* MDIO Read Data Register Bit Masks */
#  define XEL_MDIORD_RDDATA_MASK 0x0000FFFF /* Data to be Read */

/* MDIO Control Register Bit Masks */
#  define XEL_MDIOCTRL_MDIOSTS_MASK 0x00000001 /* MDIO Status Mask */
#  define XEL_MDIOCTRL_MDIOEN_MASK 0x00000008  /* MDIO Enable */

/* Use MII register 1 (MII status register) to detect PHY */
#  define PHY_DETECT_REG 1

/* Mask used to verify certain PHY features (or register
 * contents) in the register above: 0x1000: 10Mbps full
 * duplex support 0x0800: 10Mbps half duplex support 0x0008:
 * Auto-negotiation support
 */
#  define PHY_DETECT_MASK 0x1808

/* register space */
#  define TX_PING 0x0
#  define MDIO_ADDR 0x7e4
#  define MDIO_WR 0x7e8
#  define MDIO_RD 0x7ec
#  define MDIO_CTRL 0x7f0
#  define TX_PING_TPLR 0x7f4
#  define GBL_INT 0x7f8
#  define TX_PING_TSR 0x7fc
#  define TX_PONG 0x800
#  define TX_PONG_TPLR 0xff4
#  define TX_PONG_TSR 0xffc
#  define RX_PING 0x1000
#  define RX_PING_RSR 0x17fc
#  define RX_PONG 0x1800
#  define RX_PONG_RSR 0x1ffc

#  define TX_PING_BUF_END (MDIO_ADDR - 4)
#  define TX_PONG_BUF_END (TX_PONG_TPLR - 4)
#  define RX_PING_BUF_END (RX_PING_RSR - 4)
#  define RX_PONG_BUF_END (RX_PONG_RSR - 4)

/* Generic MII registers. */

#  define MII_BMCR 0x00        /* Basic mode control register */
#  define MII_BMSR 0x01        /* Basic mode status register  */
#  define MII_PHYSID1 0x02     /* PHYS ID 1		       */
#  define MII_PHYSID2 0x03     /* PHYS ID 2		       */
#  define MII_ADVERTISE 0x04   /* Advertisement control reg */
#  define MII_LPA 0x05         /* Link partner ability reg    */
#  define MII_EXPANSION 0x06   /* Expansion register */
#  define MII_CTRL1000 0x09    /* 1000BASE-T control	       */
#  define MII_STAT1000 0x0a    /* 1000BASE-T status	       */
#  define MII_ESTATUS 0x0f     /* Extended Status */
#  define MII_DCOUNTER 0x12    /* Disconnect counter	       */
#  define MII_FCSCOUNTER 0x13  /* False carrier counter       */
#  define MII_NWAYTEST 0x14    /* N-way auto-neg test reg */
#  define MII_RERRCOUNTER 0x15 /* Receive error counter       */
#  define MII_SREVISION 0x16   /* Silicon revision	       */
#  define MII_RESV1 0x17       /* Reserved...		       */
#  define MII_LBRERROR 0x18    /* Lpback, rx, bypass error  */
#  define MII_PHYADDR 0x19     /* PHY address		       */
#  define MII_RESV2 0x1a       /* Reserved...		       */
#  define MII_TPISTATUS 0x1b   /* TPI status for 10mbps */
#  define MII_NCONFIG 0x1c     /* Network interface config    */

/* Basic mode control register. */
#  define BMCR_RESV 0x003f      /* Unused...		       */
#  define BMCR_SPEED1000 0x0040 /* MSB of Speed (1000)	       */
#  define BMCR_CTST 0x0080      /* Collision test	       */
#  define BMCR_FULLDPLX 0x0100  /* Full duplex		       */
#  define BMCR_ANRESTART 0x0200 /* Auto negotiation restart    */
#  define BMCR_ISOLATE 0x0400   /* Disconnect DP83840 from MII */
#  define BMCR_PDOWN 0x0800     /* Powerdown the DP83840 */
#  define BMCR_ANENABLE 0x1000  /* Enable auto negotiation     */
#  define BMCR_SPEED100 0x2000  /* Select 100Mbps	       */
#  define BMCR_LOOPBACK 0x4000  /* TXD loopback bits	       */
#  define BMCR_RESET 0x8000     /* Reset the DP83840	       */

/* Basic mode status register. */
#  define BMSR_ERCAP 0x0001        /* Ext-reg capability	       */
#  define BMSR_JCD 0x0002          /* Jabber detected	       */
#  define BMSR_LSTATUS 0x0004      /* Link status		       */
#  define BMSR_ANEGCAPABLE 0x0008  /* Able to do auto-negotiation */
#  define BMSR_RFAULT 0x0010       /* Remote fault detected */
#  define BMSR_ANEGCOMPLETE 0x0020 /* Auto-negotiation complete   */
#  define BMSR_RESV 0x00c0         /* Unused...		       */
#  define BMSR_ESTATEN 0x0100      /* Extended Status in R15 */
#  define BMSR_100HALF2 0x0200     /* Can do 100BASE-T2 HDX */
#  define BMSR_100FULL2 0x0400     /* Can do 100BASE-T2 FDX */
#  define BMSR_10HALF                    \
    0x0800 /* Can do 10mbps, half-duplex \
            */
#  define BMSR_10FULL                                        \
    0x1000                     /* Can do 10mbps, full-duplex \
                                */
#  define BMSR_100HALF 0x2000  /* Can do 100mbps, half-duplex */
#  define BMSR_100FULL 0x4000  /* Can do 100mbps, full-duplex */
#  define BMSR_100BASE4 0x8000 /* Can do 100mbps, 4k packets  */

/* Advertisement control register. */
#  define ADVERTISE_SLCT 0x001f          /* Selector bits	       */
#  define ADVERTISE_CSMA 0x0001          /* Only selector supported     */
#  define ADVERTISE_10HALF 0x0020        /* Try for 10mbps half-duplex  */
#  define ADVERTISE_1000XFULL 0x0020     /* Try for 1000BASE-X full-duplex */
#  define ADVERTISE_10FULL 0x0040        /* Try for 10mbps full-duplex  */
#  define ADVERTISE_1000XHALF 0x0040     /* Try for 1000BASE-X half-duplex */
#  define ADVERTISE_100HALF 0x0080       /* Try for 100mbps half-duplex */
#  define ADVERTISE_1000XPAUSE 0x0080    /* Try for 1000BASE-X pause */
#  define ADVERTISE_100FULL 0x0100       /* Try for 100mbps full-duplex */
#  define ADVERTISE_1000XPSE_ASYM 0x0100 /* Try for 1000BASE-X asym pause */
#  define ADVERTISE_100BASE4 0x0200      /* Try for 100mbps 4k packets  */
#  define ADVERTISE_PAUSE_CAP 0x0400     /* Try for pause	       */
#  define ADVERTISE_PAUSE_ASYM 0x0800    /* Try for asymetric pause */
#  define ADVERTISE_RESV 0x1000          /* Unused...		       */
#  define ADVERTISE_RFAULT 0x2000        /* Say we can detect faults    */
#  define ADVERTISE_LPACK 0x4000         /* Ack link partners response  */
#  define ADVERTISE_NPAGE 0x8000         /* Next page bit	       */

#  define ADVERTISE_FULL (ADVERTISE_100FULL | ADVERTISE_10FULL | ADVERTISE_CSMA)
#  define ADVERTISE_ALL                                        \
    (ADVERTISE_10HALF | ADVERTISE_10FULL | ADVERTISE_100HALF | \
        ADVERTISE_100FULL)

/* Link partner ability register. */
#  define LPA_SLCT 0x001f      /* Same as advertise selector  */
#  define LPA_10HALF 0x0020    /* Can do 10mbps half-duplex */
#  define LPA_1000XFULL 0x0020 /* Can do 1000BASE-X full-duplex */
#  define LPA_10FULL 0x0040    /* Can do 10mbps full-duplex */
#  define LPA_1000XHALF 0x0040 /* Can do 1000BASE-X half-duplex */
#  define LPA_100HALF                                         \
    0x0080                      /* Can do 100mbps half-duplex \
                                 */
#  define LPA_1000XPAUSE 0x0080 /* Can do 1000BASE-X pause     */
#  define LPA_100FULL                                              \
    0x0100                           /* Can do 100mbps full-duplex \
                                      */
#  define LPA_1000XPAUSE_ASYM 0x0100 /* Can do 1000BASE-X pause asym*/
#  define LPA_100BASE4 0x0200        /* Can do 100mbps 4k packets   */
#  define LPA_PAUSE_CAP 0x0400       /* Can pause		       */
#  define LPA_PAUSE_ASYM 0x0800      /* Can pause asymetrically     */
#  define LPA_RESV 0x1000            /* Unused...		       */
#  define LPA_RFAULT 0x2000          /* Link partner faulted */
#  define LPA_LPACK 0x4000           /* Link partner acked us       */
#  define LPA_NPAGE 0x8000           /* Next page bit	       */

#  define LPA_DUPLEX (LPA_10FULL | LPA_100FULL)
#  define LPA_100 (LPA_100FULL | LPA_100HALF | LPA_100BASE4)

/* Expansion register for auto-negotiation. */
#  define EXPANSION_NWAY 0x0001        /* Can do N-way auto-nego      */
#  define EXPANSION_LCWP 0x0002        /* Got new RX page code word   */
#  define EXPANSION_ENABLENPAGE 0x0004 /* This enables npage words */
#  define EXPANSION_NPCAPABLE 0x0008   /* Link partner supports npage */
#  define EXPANSION_MFAULTS 0x0010     /* Multiple faults detected    */
#  define EXPANSION_RESV 0xffe0        /* Unused...		       */

#  define ESTATUS_1000_XFULL 0x8000 /* Can do 1000BX Full */
#  define ESTATUS_1000_XHALF 0x4000 /* Can do 1000BX Half */
#  define ESTATUS_1000_TFULL 0x2000 /* Can do 1000BT Full */
#  define ESTATUS_1000_THALF 0x1000 /* Can do 1000BT Half */

/* N-way test register. */
#  define NWAYTEST_RESV1 0x00ff    /* Unused...		       */
#  define NWAYTEST_LOOPBACK 0x0100 /* Enable loopback for N-way   */
#  define NWAYTEST_RESV2 0xfe00    /* Unused...		       */

/* 1000BASE-T Control register */
#  define ADVERTISE_1000FULL 0x0200 /* Advertise 1000BASE-T full duplex */
#  define ADVERTISE_1000HALF 0x0100 /* Advertise 1000BASE-T half duplex */

/* 1000BASE-T Status register */
#  define LPA_1000LOCALRXOK 0x2000 /* Link partner local receiver status */
#  define LPA_1000REMRXOK 0x1000   /* Link partner remote receiver status */
#  define LPA_1000FULL                            \
    0x0800 /* Link partner 1000BASE-T full duplex \
            */
#  define LPA_1000HALF                            \
    0x0400 /* Link partner 1000BASE-T half duplex \
            */

/* Flow control flags */
#  define FLOW_CTRL_TX 0x01
#  define FLOW_CTRL_RX 0x02

char *eth_iface = NULL;

static u8 eth_mac_addr[ENET_ADDR_LENGTH] = {0x00, 0x00, 0x5E, 0x00, 0xFA, 0xCE};
static u32 eth_ip_addr = 0;

static struct sockaddr_ll eth_sll;
static u8 iface_mac_addr[ENET_ADDR_LENGTH];
static u32 iface_ip_addr = 0;

typedef struct {
  u32 regnum : 5;  /* phy register address */
  u32 phyaddr : 5; /* phy device address */
  u32 op : 1;      /* 0: write access, 1: read access */
  u32 _1 : 21;
} mdio_addr_t;

typedef struct {
  u32 status : 1; /* 0: mdio is ready, 1: mdio is not ready
                   */
  u32 _1 : 2;
  u32 enable : 1; /* 0: disable mdio, 1: enable mdio */
  u32 _2 : 28;
} mdio_ctrl_t;

struct emaclite_regs {
  u32 tx_ping; /* 0x0 - TX Ping buffer */
  u32 reserved1[504];
  u32 mdioaddr;     /* 0x7e4 - MDIO Address Register */
                    /* -----------------------------
                     * 31 .. 11 : reserved
                     *       10 : OP, 0 for write access
                     *                1 for read access
                     *  9 .. 5  : PHY address
                     *  4 .. 0  : register address
                     */
  u32 mdiowr;       /* 0x7e8 - MDIO Write Data Register */
                    /* --------------------------------
                     * 31 .. 16 : reserved
                     * 15 .. 0  : write data
                     */
  u32 mdiord;       /* 0x7ec - MDIO Read Data Register */
                    /* --------------------------------
                     * 31 .. 16 : reserved
                     * 15 .. 0  : read data
                     */
  u32 mdioctrl;     /* 0x7f0 - MDIO Control Register */
                    /* -----------------------------
                     * 31 ..  4 : reserved
                     *        3 : MDIO enable bit
                     *  2 ..  1 : reserved
                     *        0 : 0 ready to accept a new MDIO accept
                     *            1 not ready, write 1 to request
                     */
  u32 tx_ping_tplr; /* 0x7f4 - Tx packet length */
                    /* ------------------------
                     *   31 .. 16 : reserved
                     *   15 ..  0 : 16 bits of frame length
                     */

  u32 global_interrupt; /* 0x7f8 - Global interrupt enable */

  u32 tx_ping_tsr; /* 0x7fc - Tx status */
                   /* -----------------
                    * 31 .. 5 : reserved
                    *       4 : internal loopback enable
                    *       3 : interrupt enable
                    *       2 : reserved
                    *       1 : mac address program bit
                    *       0 : buffer in progress indicator
                    */
  u32 tx_pong;     /* 0x800 - TX Pong buffer */
  u32 reserved2[508];
  u32 tx_pong_tplr; /* 0xff4 - Tx packet length */
                    /* ------------------------
                     *   31 .. 16 : reserved
                     *   15 ..  0 : 16 bits of frame length
                     */
  u32 reserved3;    /* 0xff8 */
  u32 tx_pong_tsr;  /* 0xffc - Tx status */
                    /* -----------------
                     * 31 .. 2 : reserved
                     *       1 : mac address program bit
                     *       0 : buffer in progress indicator
                     */
  u32 rx_ping;      /* 0x1000 - Receive Buffer */
  u32 reserved4[510];
  u32 rx_ping_rsr; /* 0x17fc - Rx status */
                   /* -----------------
                    * 31 .. 4 : reserved
                    *       3 : interrupt enable
                    *  2 .. 1 : reserved
                    *       0 : buffer in progress indicator
                    */
  u32 rx_pong;     /* 0x1800 - Receive Buffer */
  u32 reserved5[510];
  u32 rx_pong_rsr; /* 0x1ffc - Rx status */
                   /* -----------------
                    * 31 .. 1 : reserved
                    *       0 : buffer in progress indicator
                    */
};

static struct emaclite_regs regs;
static u16 phy_regs[32][32]; /* phy_regs[phy][reg] */
static int mac_socket = -1;

#  define ACTIVE_PHY 1

static u32 reverse_table[256];

// static u8 mac_Tx_frame[2048];

u64 Reflect(u64 ref, u8 ch) {
  int i;
  u64 value = 0;
  for (i = 1; i < (ch + 1); i++) {
    if (ref & 1) value |= 1 << (ch - i);
    ref >>= 1;
  }
  return value;
}

static void gen_normal_table(u32 *table) {
  u32 gx = 0x04c11db7;
  u32 temp = 0;
  // u32 crc = 0;
  for (int i = 0; i <= 0xFF; i++) {
    temp = Reflect(i, 8);
    table[i] = temp << 24;
    for (int j = 0; j < 8; j++) {
      unsigned long int t1, t2;
      unsigned long int flag = table[i] & 0x80000000;
      t1 = (table[i] << 1);
      if (flag == 0)
        t2 = 0;
      else
        t2 = gx;
      table[i] = t1 ^ t2;
    }
    // crc = table[i];
    table[i] = Reflect(table[i], 32);
  }
}

u32 reverse_table_crc(const u8 *data, u32 len, u32 *table) {
  u32 crc = 0xffffffff;
  const u8 *p = data;
  int i;
  for (i = 0; i < len; i++) crc = table[(crc ^ (*(p + i))) & 0xff] ^ (crc >> 8);
  return ~crc;
}

void hexdump(const u8 *data, const int len) {
  for (int i = 0; i < len; i += 16) {
    printf("%02x: ", i);
    for (int j = i; j < len && j < i + 16; j++) printf("%02x ", data[j]);
    printf("\n");
  }
}

#  if 0
static void eth_packet_check_crc(u8 *data, const int len) {
  assert (len >= 16);
  u32 crc = reverse_table_crc(&data[12], len - 4, reverse_table);
  printf("%08x, %08x\n", crc, *(u32 *)&data[len - 4]);
  assert (*(u32 *)&data[len - 4] == crc);
}

static void eth_packet_modify_crc(u8 *data, const int len) {
  u32 crc = reverse_table_crc(&data[12], len - 4, reverse_table);
  *(u32 *)&data[len - 4] = crc;
}
#  endif

static void ip_packet_modify_checksum(u8 *data, const int len) {
  u32 checksum = 0;
  *(u16 *)&data[24] = 0;
  for (int i = 14; i < 34; i += 2) checksum += *(u16 *)&data[i];
  checksum = (checksum >> 16) + (checksum & 0xFFFF);
  // printf("check sum is %04x\n", checksum);
  *(u16 *)&data[24] = ~checksum;
}

const char *ip_ntoa(u32 ip) {
  static char s[128];
  u8 *p = (u8 *)&ip;
  sprintf(s, "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
  return s;
}

static void sender_modify_packet(u8 *data, const int len) {
  /* copy the destination addr */
  memcpy(&eth_sll.sll_addr, &data[0], ENET_ADDR_LENGTH);
  /* override the source addr */
  memcpy(&data[ENET_ADDR_LENGTH], iface_mac_addr, ENET_ADDR_LENGTH);

  int protocol = ntohs(*(u16 *)&data[12]);
  switch (protocol) {
  case ETH_P_IP: {
    break;
    memcpy(&data[0x1a], &iface_ip_addr, 4);
    ip_packet_modify_checksum(data, len);
  } break;
  case ETH_P_ARP:
    memcpy(&data[0x16], &iface_mac_addr, ENET_ADDR_LENGTH);
    memcpy(&eth_ip_addr, &data[0x1c], 4);
    printf("set eth_ip_addr to %s\n", ip_ntoa(eth_ip_addr));
    memcpy(&data[0x1c], &iface_ip_addr, 4);
    break;
  }
  // eth_packet_modify_crc(data, len);
}

static void recver_modify_packet(u8 *data, const int len) {
  /* copy the eth addr */
  memcpy(data, eth_mac_addr, ENET_ADDR_LENGTH);
  /* 0x1a .. 0x1d is ip.src.ip, 0x1e .. 0x21 is ip.dst.ip */
  /* 0x16 .. 0x1b is arp.src.mac, 0x1c .. 0x1f is arp.src.ip */
  /* 0x20 .. 0x25 is arp.dst.mac, 0x26 .. 0x29 is arp.dst.ip */

  int protocol = ntohs(*(u16 *)&data[12]);
  switch (protocol) {
  case ETH_P_IP: {
    break;
    memcpy(&data[0x1e], &eth_ip_addr, 4);
    ip_packet_modify_checksum(data, len);
  } break;
  case ETH_P_ARP:
    memcpy(&data[0x20], &eth_mac_addr, ENET_ADDR_LENGTH);
    memcpy(&data[0x26], &eth_ip_addr, 4);
    break;
  }
  // eth_packet_modify_crc(data, len);
}

static void send_data(const u8 *data, const int len) {
  static u8 eth_frame[2048];
  assert(len < sizeof(eth_frame));
  memcpy(eth_frame, data, len);

  pcap_write(pcap, data, len);
  pcap_flush(pcap);

  sender_modify_packet(eth_frame, len);

  eth_sll.sll_protocol = *(u16 *)&eth_frame[12];

  // printf("ssl_protocol: %x\n", eth_sll.sll_protocol);
  //  printf("type is %x %x\n", htons(ETH_P_ARP), htons(ETH_P_IP));

  /* send the eth_frame */
  sendto(mac_socket, &eth_frame[0], len, 0, (struct sockaddr *)&eth_sll,
      sizeof(eth_sll));
  // printf("send.nbytes is %d\n", nbytes);
  // hexdump(&eth_frame[0], len);
}

static int recv_data(u8 *to, const int maxlen) {
  // printf("try receive data\n");
  int nbytes = recvfrom(mac_socket, to, maxlen, 0, NULL, NULL);

  recver_modify_packet(to, nbytes);
  pcap_write(pcap, to, nbytes);
  pcap_flush(pcap);
#  if 0
  printf("recv.nbytes is %d\n", nbytes);
  hexdump(to, nbytes);
#  endif
  return nbytes;
}

static void mac_init_socket() {
  /* table to accerate the crc calculation */
  gen_normal_table(reverse_table);

  /* init the socket */
  mac_socket = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  Assert(mac_socket > 0, "init raw socket failed, please run me with sudo");

  struct ifreq eth_req;
  Assert(eth_iface, "please specify net card with --iface");
  strncpy(eth_req.ifr_name, eth_iface, IFNAMSIZ);
  if (ioctl(mac_socket, SIOCGIFINDEX, &eth_req) == -1) {
    perror("ioctl");
    exit(0);
  }

  eth_sll.sll_ifindex = eth_req.ifr_ifindex;
  eth_sll.sll_family = AF_PACKET;
  eth_sll.sll_halen = ETHER_ADDR_LEN;

  /* get mac address */
  if (ioctl(mac_socket, SIOCGIFHWADDR, &eth_req) == -1) {
    perror("ioctl");
    abort();
  } else {
    memcpy(iface_mac_addr, eth_req.ifr_hwaddr.sa_data, ENET_ADDR_LENGTH);
  }

  /* get ip address */
  if (ioctl(mac_socket, SIOCGIFADDR, &eth_req) == -1) {
    perror("ioctl");
    abort();
  } else {
    struct sockaddr_in *iface_ip = (void *)&eth_req.ifr_addr;
    iface_ip_addr = iface_ip->sin_addr.s_addr;
  }

#  if 0
  printf("mac addr of %s:", eth_iface);
  for (int i = 0; i < 6; i++) { printf(" %02x", iface_mac_addr[i]); }
  printf("\n");
#  endif
}

static void mac_init_phy_regs() {
  /* init phy regs */
  phy_regs[ACTIVE_PHY][MII_PHYSID1] = 0x181;
  phy_regs[ACTIVE_PHY][MII_PHYSID2] = 0xb8a0;
  phy_regs[ACTIVE_PHY][PHY_DETECT_REG] = PHY_DETECT_MASK;

  phy_regs[ACTIVE_PHY][MII_BMCR] =
      BMCR_SPEED100 | BMCR_ANENABLE | BMCR_FULLDPLX;
  phy_regs[ACTIVE_PHY][MII_BMSR] = BMSR_100FULL | BMSR_100HALF | BMSR_10FULL |
                                   BMSR_10HALF | BMSR_ANEGCAPABLE | BMSR_ERCAP |
                                   0x40;
  phy_regs[ACTIVE_PHY][MII_ADVERTISE] = ADVERTISE_100FULL | ADVERTISE_100HALF |
                                        ADVERTISE_10FULL | ADVERTISE_10HALF |
                                        ADVERTISE_CSMA;
  phy_regs[ACTIVE_PHY][MII_LPA] = LPA_LPACK | LPA_PAUSE_ASYM | LPA_PAUSE_CAP |
                                  LPA_1000XPAUSE_ASYM | LPA_100HALF |
                                  LPA_10FULL | LPA_10HALF | 0x1;
}

static void mac_init() {
  pcap = pcap_open("build/packets.pcap");
  mac_init_socket();
  mac_init_phy_regs();
}

static void mii_transaction() {
  mdio_addr_t *mdio_addr = (void *)&(regs.mdioaddr);
  u32 phy = mdio_addr->phyaddr;
  u32 reg = mdio_addr->regnum;
  if (mdio_addr->op == 0) {
    /* write */
    u16 data = regs.mdiowr;
    if (phy != ACTIVE_PHY) return;
    switch (reg) {
    case MII_BMCR:
      if (data & BMCR_RESET) {
        phy_regs[phy][MII_BMSR] |= (BMSR_ANEGCOMPLETE | BMSR_LSTATUS);
      } else {
        phy_regs[phy][reg] = data;
      }
      break;
    default: CPUAssert(0, "unsupported MII reg %d write access\n", reg);
    }
  } else {
    /* read */
    regs.mdiord = phy_regs[phy][reg];
  }
}

static uint32_t mac_read(paddr_t addr, int len) {
  int recvlen = 0;
  switch (addr) {
  case TX_PING_TSR: return regs.tx_ping_tsr;
  case TX_PONG_TSR: return regs.tx_pong_tsr;
  case RX_PING ... RX_PING_BUF_END: return ((u32 *)&regs)[addr / 4];
  case RX_PONG ... RX_PONG_BUF_END: return ((u32 *)&regs)[addr / 4];
  case RX_PING_RSR:
    recvlen = recv_data((u8 *)&regs.rx_ping, 0x500);
    if (recvlen > 0) { regs.rx_ping_rsr |= XEL_RSR_RECV_DONE_MASK; }
    return regs.rx_ping_rsr;
  case RX_PONG_RSR:
    recvlen = recv_data((u8 *)&regs.rx_pong, 0x500);
    if (recvlen > 0) { regs.rx_pong_rsr |= XEL_RSR_RECV_DONE_MASK; }
    return regs.rx_pong_rsr;
  case MDIO_RD: return regs.mdiord;
  case MDIO_CTRL: return regs.mdioctrl;
  default:
    CPUAssert(false, "mac: address(0x%08x) is not readable", addr);
    break;
  }
  return 0;
}

static void mac_write(paddr_t addr, int len, uint32_t data) {
  switch (addr) {
  case TX_PING_TSR:
    regs.tx_ping_tsr = data;
    if (regs.tx_ping_tsr & XEL_TSR_XMIT_BUSY_MASK) {
      if (regs.tx_ping_tsr & XEL_TSR_PROGRAM_MASK) {
        memcpy(eth_mac_addr, &regs.tx_ping, regs.tx_ping_tplr);
        regs.tx_ping_tsr &= ~XEL_TSR_PROG_MAC_ADDR;
      } else {
        /* send ping packet */
        send_data((u8 *)&regs.tx_ping, regs.tx_ping_tplr);
        regs.tx_ping_tsr &= ~XEL_TSR_XMIT_BUSY_MASK;
      }
    }
    break;
  case TX_PONG_TSR:
    regs.tx_pong_tsr = data;
    if (regs.tx_pong_tsr & XEL_TSR_XMIT_BUSY_MASK) {
      if (regs.tx_pong_tsr & XEL_TSR_PROGRAM_MASK) {
        memcpy(eth_mac_addr, &regs.tx_pong, regs.tx_pong_tplr);
        regs.tx_pong_tsr &= ~XEL_TSR_PROG_MAC_ADDR;
      } else {
        /* send pong packet */
        send_data((u8 *)&regs.tx_pong, regs.tx_pong_tplr);
        regs.tx_pong_tsr &= ~XEL_TSR_XMIT_BUSY_MASK;
      }
    }
    break;
  case TX_PING_TPLR: regs.tx_ping_tplr = data; break;
  case TX_PONG_TPLR: regs.tx_pong_tplr = data; break;
  case TX_PING ... TX_PING_BUF_END: ((u32 *)&regs)[addr / 4] = data; break;
  case TX_PONG ... TX_PONG_BUF_END: ((u32 *)&regs)[addr / 4] = data; break;
  case RX_PING_RSR: regs.rx_ping_rsr = data; break;
  case RX_PONG_RSR: regs.rx_pong_rsr = data; break;
  case MDIO_CTRL:
    regs.mdioctrl = data;
    if (regs.mdioctrl & XEL_MDIOCTRL_MDIOSTS_MASK) {
      mii_transaction();
      regs.mdioctrl &= ~XEL_MDIOCTRL_MDIOSTS_MASK;
    }
    break;
  case MDIO_ADDR: regs.mdioaddr = data; break;
  case MDIO_WR: regs.mdiowr = data; break;
  default:
    CPUAssert(false, "mac: address(0x%08x) is not writable", addr);
    break;
  }
}

DEF_DEV(mac_dev) = {
    .name = "MAC",
    .start = MAC_ADDR,
    .end = MAC_ADDR + MAC_SIZE,
    .init = mac_init,
    .read = mac_read,
    .peek = mac_read,
    .write = mac_write,
    .map = NULL,
};
#endif