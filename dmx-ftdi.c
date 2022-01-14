#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <libftdi1/ftdi.h>
#include <pthread.h>

#define DMX_UNIX_SOCKET "/tmp/dmx.sock"

#define USB_VENDOR_ID   0x0403  // FTDI USB Vendor
#define USB_PRODUCT_ID  0x6001  // FTDI USB Product

// Enttec Protocol used by Eurolite USB-DMX512-PRO MK2
#define ENTTEC_PRO_DMX_ZERO      0x00
#define ENTTEC_PRO_SEND_DMX_RQ   0x06
#define ENTTEC_PRO_START_OF_MSG  0x7E
#define ENTTEC_PRO_END_OF_MSG    0xE7

#define FTDI_FIELD(x)    x, sizeof(x)

#define debug printf

typedef struct dmx_t {
    struct ftdi_context *kntxt; // context
    int bits;
    int stop;
    int parity;
    int baudrate;

    pthread_t worker;
    pthread_mutex_t lock;
    char univers[512];

} dmx_t;

void diep(char *str) {
    fprintf(stderr, "[-] %s: %s\n", str, strerror(errno));
}

int dmx_ftdi_error(dmx_t *iface) {
    fprintf(stderr, "[-] ftdi: %s\n", ftdi_get_error_string(iface->kntxt));
    return 1;
}

dmx_t *dmx_open() {
    dmx_t *dmx = (dmx_t *) malloc(sizeof(dmx_t));

    if(!dmx)
        diep("dmx: malloc");

    debug("[+] initializing\n");

    // initialize ftdi context
    if(!(dmx->kntxt = ftdi_new()))
        diep("ftdi: new");

    return dmx;
}

void dmx_free(dmx_t *dmx) {
    free(dmx);
}

int dmx_interface_lookup(dmx_t *iface) {
    struct ftdi_device_list *devlist = NULL;
    struct ftdi_device_list *dev;

	if(ftdi_usb_find_all(iface->kntxt, &devlist, USB_VENDOR_ID, USB_PRODUCT_ID) < 0)
        return dmx_ftdi_error(iface);

    // using a loop but using the first one only anyway
    for(dev = devlist; dev; dev = dev->next) {
        char manufacturer[128];
        char description[128];
        char serial[128];

        if(ftdi_usb_get_strings(iface->kntxt, dev->dev, FTDI_FIELD(manufacturer), FTDI_FIELD(description), FTDI_FIELD(serial)) < 0)
            return dmx_ftdi_error(iface);

        debug("[+] =========================================\n");
        debug("[+] adapter manufacturer: %s\n", manufacturer);
        debug("[+] adapter description : %s\n", description);
        debug("[+] adapter serial      : %s\n", serial);
        debug("[+] =========================================\n");

        if(ftdi_usb_open_dev(iface->kntxt, dev->dev) < 0)
            return dmx_ftdi_error(iface);

        break;
    }

    ftdi_list_free(&devlist);

    return 0;
}

// open the first FTDI interface found
int dmx_interface_setup(dmx_t *iface) {
    // interface settings
    iface->bits = BITS_8;
    iface->stop = STOP_BIT_2;
    iface->parity = NONE;
    iface->baudrate = 250000;

    debug("[+] setup: reset device\n");
    if(ftdi_usb_reset(iface->kntxt) < 0)
        return dmx_ftdi_error(iface);

    debug("[+] setup: set baudrate: %d\n", iface->baudrate);
    if(ftdi_set_baudrate(iface->kntxt, iface->baudrate) < 0)
        return dmx_ftdi_error(iface);

    debug("[+] setup: configuring line\n");
    if(ftdi_set_line_property2(iface->kntxt, iface->bits, iface->stop, iface->parity, BREAK_OFF) < 0)
        return dmx_ftdi_error(iface);

    debug("[+] setup: configuring flow control\n");
    if(ftdi_setflowctrl(iface->kntxt, SIO_DISABLE_FLOW_CTRL) < 0)
        return dmx_ftdi_error(iface);

    debug("[+] setup: disabling request to send (rts)\n");
    if(ftdi_setrts(iface->kntxt, 0) < 0)
        return dmx_ftdi_error(iface);

    debug("[+] setup: flushing device\n");
    ftdi_tcioflush(iface->kntxt);

    return 0;
}

// length should be always 512
int dmx_interface_send(dmx_t *iface, char *univers, size_t length) {
    unsigned char *buffer = malloc(length + 8);
    ssize_t datalen = length + 6;

    #if 0
    // not needed for Pro version
    if(ftdi_set_line_property2(iface->kntxt, iface->bits, iface->stop, iface->parity, BREAK_ON) < 0)
        return dmx_ftdi_error(iface);

    if(ftdi_set_line_property2(iface->kntxt, iface->bits, iface->stop, iface->parity, BREAK_OFF) < 0)
        return dmx_ftdi_error(iface);
    #endif

    buffer[0] = ENTTEC_PRO_START_OF_MSG;
    buffer[1] = ENTTEC_PRO_SEND_DMX_RQ;
    buffer[2] = (length + 1) & 0xff;        // data length lsb
    buffer[3] = ((length + 1) >> 8) & 0xff; // data length msb
    buffer[4] = ENTTEC_PRO_DMX_ZERO;

    // copy univers value
    memcpy(buffer + 5, univers, length);

    buffer[5 + length] = ENTTEC_PRO_END_OF_MSG; // end of message

    debug("[+] protocol: writing univers (%lu bytes)\n", datalen);
    if(ftdi_write_data(iface->kntxt, buffer, datalen) != datalen)
        return dmx_ftdi_error(iface);

    // take some rest, this make signal more smooth
    usleep(20000);

    free(buffer);

    return 0;
}

int dmx_interface_close(dmx_t *iface) {
    debug("[+] closing interface\n");
    ftdi_free(iface->kntxt);

    return 0;
}

void dmx_set(char *univers, int channel, int value) {
    univers[channel - 1] = value;
}

void *dmx_interface_worker(void *_dmx) {
    dmx_t *dmx = (dmx_t *) _dmx;
    char univers[512];

    while(1) {
        // copy a local version of univers, so we lock the univers
        // only the shortest time possible
        pthread_mutex_lock(&dmx->lock);
        memcpy(univers, dmx->univers, sizeof(univers));
        pthread_mutex_unlock(&dmx->lock);

        // we apply univers value, we can take our time now
        dmx_interface_send(dmx, univers, sizeof(univers));
    }
}

int dmx_interface_start(dmx_t *dmx) {
    if(pthread_mutex_init(&dmx->lock, NULL))
        diep("pthread: mutex: init");

    if(pthread_create(&dmx->worker, NULL, dmx_interface_worker, dmx))
        diep("pthread: create");

    return 0;
}

int network_handler(dmx_t *dmx) {
	struct sockaddr_un addr;
	struct sockaddr_un from;
	socklen_t fromlen = sizeof(from);
	char buff[8192];
    int fd;
	int len;

    if((fd = socket(PF_UNIX, SOCK_DGRAM, 0)) < 0)
        diep("socket");

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    strcpy(addr.sun_path, DMX_UNIX_SOCKET);
    unlink(DMX_UNIX_SOCKET);

    if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        diep("bind");

    printf("[+] network: waiting frames\n");

	while((len = recvfrom(fd, buff, 8192, 0, (struct sockaddr *) &from, &fromlen)) > 0) {
		printf("[+] network: received frame length: %u\n", len);

        if(len == 512) {
            pthread_mutex_lock(&dmx->lock);
            memcpy(dmx->univers, buff, 512);
            pthread_mutex_unlock(&dmx->lock);
        }
    }

    close(fd);

    return 0;
}

int main() {
    dmx_t *dmx = dmx_open();

    if(dmx_interface_lookup(dmx))
        return 1;

    if(dmx_interface_setup(dmx))
        return 1;

    // start dmx worker
    dmx_interface_start(dmx);

    // waiting for network event
    network_handler(dmx);

    // pthread_mutex_destroy(&lock);

    if(dmx_interface_close(dmx))
        return 1;

    dmx_free(dmx);

    return 0;
}
