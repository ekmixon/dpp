/*
 * (c) Copyright 2016, 2017, 2018 Hewlett Packard Enterprise Development LP
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, 
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>              /* DELETE ME WITH SCAN STUFF */
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/bpf.h>
#include <net/route.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include "ieee802_11.h"
#include "tlv.h"
#include "service.h"
#include "common.h"
#include "pkex.h"
#include "dpp.h"

struct conversation {
    TAILQ_ENTRY(conversation) entry;
    dpp_handle handle;
    timerid t0;
    int fd;
};
TAILQ_HEAD(bar, conversation) conversations;

service_context srvctx;
char bootstrapfile[80];

static void
dump_buffer (unsigned char *buf, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        if (i && (i%4 == 0)) {
            printf(" ");
        }
        if (i && (i%32 == 0)) {
            printf("\n");
        }
        printf("%02x", buf[i]);
    }
    printf("\n");
}

static void
print_buffer (char *str, unsigned char *buf, int len)
{
    printf("%s:\n", str);
    dump_buffer(buf, len);
    printf("\n");
}

static void
srvfail (int fd, void *data)
{
    struct conversation *conv = NULL;

    TAILQ_FOREACH(conv, &conversations, entry) {
        if (fd == conv->fd) {
            TAILQ_REMOVE(&conversations, conv, entry);
            break;
        }
    }
    srv_rem_input(srvctx, fd);
    close(fd);
    if (conv != NULL) {
        free(conv);
    }
    return;
}

void
delete_conv (timerid t, void *data)
{
    struct conversation *conv = (struct conversation *)data;
    
    printf("deleting conversation...\n");
    srv_rem_input(srvctx, conv->fd);
    TAILQ_REMOVE(&conversations, conv, entry);
    if (conv->handle) {
        dpp_free_peer(conv->handle);
    }
    close(conv->fd);
    free(conv);

    return;
}

static void
message_from_relay (int fd, void *data)
{
    struct conversation *conv = (struct conversation *)data;
    dpp_action_frame *dpp;
    unsigned char buf[3000], pmk[PMK_LEN], pmkid[PMKID_LEN];
    int framesize, rlen;
    uint32_t netlen;

    memset(buf, 0, sizeof(buf));
    if (read(fd, (char *)&netlen, sizeof(uint32_t)) < 0) {
        fprintf(stderr, "can't read message from relay!\n");
        srv_rem_timeout(srvctx, conv->t0);
        srv_add_timeout(srvctx, SRV_USEC(1), delete_conv, conv);
        return;
    }
    netlen = ntohl(netlen);
    if (netlen == 0) {
        fprintf(stderr, "failed to read from peer!\n");
        srv_rem_timeout(srvctx, conv->t0);
        srv_add_timeout(srvctx, SRV_USEC(1), delete_conv, conv);
        return;
    }
    if (netlen > sizeof(buf)) {
        fprintf(stderr, "overflow attack by relay/client! Not gonna read in %d bytes\n",
                netlen);
        srv_rem_timeout(srvctx, conv->t0);
        srv_add_timeout(srvctx, SRV_USEC(1), delete_conv, conv);
        return;
    }
    
    framesize = 0;
    while (netlen) {
        if ((rlen = read(fd, (buf + framesize), netlen)) < 1) {
            fprintf(stderr, "can't read message from relay!\n");
            srv_rem_timeout(srvctx, conv->t0);
            srv_add_timeout(srvctx, SRV_USEC(1), delete_conv, conv);
            return;
        }
        framesize += rlen;
        netlen -= rlen;
    }
    
    printf("read %d byte message from relay!\n", framesize);

    switch (buf[0]) {
        case PUB_ACTION_VENDOR:
            /* 
             * PKEX, DPP Auth, and DPP Discovery
             */
            dpp = (dpp_action_frame *)&buf[1];
            switch (dpp->frame_type) {
                /*
                 * DPP Auth
                 */
                case DPP_SUB_AUTH_REQUEST:
                case DPP_SUB_AUTH_RESPONSE:
                case DPP_SUB_AUTH_CONFIRM:
                    printf("DPP auth message...\n");
                    if (process_dpp_auth_frame(&buf[1], framesize - 1, conv->handle) < 0) {
                        fprintf(stderr, "error processing DPP Auth frame\n");
                    }
                    break;
                    /*
                     * DPP Discovery
                     */
                case DPP_SUB_PEER_DISCOVER_REQ:
                    printf("DPP discovery request...\n");
                    if (process_dpp_discovery_frame(&buf[1], framesize - 1,
                                                    (unsigned char)conv->handle, pmk, pmkid) < 0) {
                        fprintf(stderr, "error processing DPP Discovery frame\n");
                    }
                    break;
                case DPP_SUB_PEER_DISCOVER_RESP:
                    /*
                     * shouldn't happen since we don't send DPP discovery requests....
                     */
                    printf("DPP discovery response...\n");
                    if (process_dpp_discovery_frame(&buf[1], framesize - 1,
                                                    (unsigned char)conv->handle, pmk, pmkid) < 0) {
                        fprintf(stderr, "error processing DPP Discovery frame\n");
                    }
                    break;
                    /*
                     * PKEX
                     */
                case PKEX_SUB_EXCH_REQ:
                case PKEX_SUB_EXCH_RESP:
                case PKEX_SUB_COM_REV_REQ:
                case PKEX_SUB_COM_REV_RESP:
                    printf("PKEX message...\n");
                    if (process_pkex_frame(&buf[1], framesize - 1, conv->handle) < 0) {
                        fprintf(stderr, "error processing PKEX frame\n");
                    }
                    break;
                case DPP_CONFIG_RESULT:
                    if (process_dpp_config_frame(BAD_DPP_SPEC_MESSAGE, &buf[1], framesize - 1,
                                                 (unsigned char)conv->handle) < 0) {
                        fprintf(stderr, "error processing DPP Config frame\n");
                    }
                    break;
                case DPP_CHIRP:
                    fprintf(stderr, "got a chirp! Need to handle this....\n");
                    break;
                default:
                    fprintf(stderr, "unknown DPP frame %d\n", dpp->frame_type);
                    break;
            }
            break;
        case GAS_INITIAL_REQUEST:
        case GAS_INITIAL_RESPONSE:
        case GAS_COMEBACK_REQUEST:
        case GAS_COMEBACK_RESPONSE:
            /*
             * DPP Configuration protocol
             */
            printf("DPP config message...\n");
            if (process_dpp_config_frame(buf[0], &buf[1], framesize - 1, conv->handle) < 0) {
                fprintf(stderr, "error processing DPP Config frame\n");
            }
            break;
        default:
            break;
    }
}

/*
 * cons up an action frame and send it out the conversation
 */
static int
cons_action_frame (unsigned char field, dpp_handle handle,
                   char *data, int len)
{
    char buf[3000], *ptr;
    uint32_t netlen;
    struct conversation *conv = NULL;

    TAILQ_FOREACH(conv, &conversations, entry) {
        if (handle == conv->handle) {
            break;
        }
    }
    if (conv == NULL) {
        fprintf(stderr, "can't find dpp instance!\n");
        return -1;
    }
    memset(buf, 0, sizeof(buf));
    ptr = buf;

    netlen = htonl(len + 1);
    memcpy(ptr, (char *)&netlen, sizeof(uint32_t));
    ptr += sizeof(uint32_t);
    
    *ptr = field;
    ptr++;
    
    memcpy(ptr, data, len < sizeof(buf) ? len : sizeof(buf));

    printf("sending %d byte message to relay\n", len + 1);
    if (write(conv->fd, buf, len + sizeof(uint32_t) + 1) < 1) {
        fprintf(stderr, "can't send message to relay!\n");
        return -1;
    }
    return len;
}

/*
 * wrappers to send action frames
 */
int
transmit_config_frame (dpp_handle handle, unsigned char field, char *data, int len)
{
    return cons_action_frame(field, handle, data, len);
}

int
transmit_auth_frame (dpp_handle handle, char *data, int len)
{
    return cons_action_frame(PUB_ACTION_VENDOR, handle, data, len);
}

int
transmit_discovery_frame (unsigned char tid, char *data, int len)
{
    return cons_action_frame(PUB_ACTION_VENDOR, (dpp_handle)tid, data, len);
}

int
transmit_pkex_frame (unsigned char *mymac, unsigned char *peermac, char *data, int len)
{
/* we don't do PKEX in the controller yet...
    return cons_action_frame(PUB_ACTION_VENDOR, mymac, peermac, data, len);
*/
    return 1;
}

int
provision_connector (char *role, unsigned char *ssid, int ssidlen,
                     unsigned char *connector, int connlen, dpp_handle handle)
{
    printf("connector:\n%.*s\nwith ", connlen, connector);
    if (ssidlen == 1 && ssid[0] == '*') {
        printf("any SSID\n");
    } else {
        printf("SSID %.*s\n", ssidlen, ssid);
    }
    return 1;
}

#if 0
int
save_bootstrap_key (unsigned char *b64key, unsigned char *peermac)
{
    FILE *fp = NULL;
    unsigned char mac[20], existing[1024];
    int ret = -1, oc, ch;
    
    if ((fp = fopen(bootstrapfile, "r+")) == NULL) {
        fprintf(stderr, "SSS: unable to open %s as bootstrapping file\n", bootstrapfile);
        goto fin;
    }
    ret = 0;
    printf("peer's bootstrapping key (b64 encoded)\n%s\n", b64key);
    while (!feof(fp)) {
        memset(existing, 0, 1024);
        if (fscanf(fp, "%d %d %d %s %s", &ret, &oc, &ch, mac, existing) < 1) {
            break;
        }
        if (strcmp((char *)existing, (char *)b64key) == 0) {
            fprintf(stderr, "SSS: bootstrapping key is trusted already\n");
            goto fin;
        }
    }
    ret++;
    /*
     * bootstrapping file is index opclass channel macaddr key
     */
    fprintf(fp, "%d 81 6 %02x%02x%02x%02x%02x%02x %s\n", ret, 
            peermac[0], peermac[1], peermac[2], peermac[3], peermac[4], peermac[5], 
            b64key);
  fin:
    if (fp != NULL) {
        fclose(fp);
    }
    return ret;
}
#endif

int
save_bootstrap_key (pkex_handle handle, void *param)
{
    FILE *fp = NULL;
    BIO *bio = NULL;
    char newone[1024], existing[1024], b64bskey[1024];
    unsigned char *ptr, mac[2*ETH_ALEN];
    int ret = -1, oc, ch, len, octets;
    EC_KEY *peerbskey = (EC_KEY *)param;
    struct conversation *conv;
    
    TAILQ_FOREACH(conv, &conversations, entry) {
        if (handle == conv->handle) {
            break;
        }
    }
    if (conv == NULL) {
        fprintf(stderr, "can't find dpp instance!\n");
        return -1;
    }

    /*
     * get the base64 encoded EC_KEY as onerow[1]
     */
    if ((bio = BIO_new(BIO_s_mem())) == NULL) {
        fprintf(stderr, "unable to create bio!\n");
        goto fin;
    }
    (void)i2d_EC_PUBKEY_bio(bio, peerbskey);
    (void)BIO_flush(bio);
    len = BIO_get_mem_data(bio, &ptr);
    octets = EVP_EncodeBlock((unsigned char *)newone, ptr, len);
    BIO_free(bio);

    memset(b64bskey, 0, 1024);
    strncpy(b64bskey, newone, octets);

    if ((fp = fopen(bootstrapfile, "r+")) == NULL) {
        fprintf(stderr, "SSS: unable to open %s as bootstrapping file\n", bootstrapfile);
        goto fin;
    }
    ret = 0;
    printf("peer's bootstrapping key (b64 encoded)\n%s\n", b64bskey);
    while (!feof(fp)) {
        memset(existing, 0, 1024);
        if (fscanf(fp, "%d %d %d %s %s", &ret, &oc, &ch, mac, existing) < 1) {
            break;
        }
        if (strcmp((char *)existing, (char *)b64bskey) == 0) {
            fprintf(stderr, "SSS: bootstrapping key is trusted already\n");
//            goto fin;
        }
    }
    ret++;
    /*
     * bootstrapping file is index opclass channel macaddr key
     * but the controller doesn't care about opclass and channel 
     * and doesn't know anything about MAC addresses....
     */
    fprintf(fp, "%d 0 0 ffffffffffff %s\n", ret, b64bskey);

  fin:
    if (fp != NULL) {
        fclose(fp);
    }
    return ret;
}

void
new_connection (int fd, void *data)
{
    struct sockaddr_in *serv = (struct sockaddr_in *)data;
    struct conversation *conv;
    int sd, rlen, framesize, ret, opclass, channel, asn1len, found = 0;
    uint32_t netlen;
    char pkey[200];
    unsigned int clen, mdlen = SHA256_DIGEST_LENGTH;
    unsigned char buf[3000], mac[20], keyasn1[1024], keyhash[SHA256_DIGEST_LENGTH];
    dpp_action_frame *dpp;
    EVP_MD_CTX *mdctx = NULL;
    TLV *rhash;
    FILE *fp;
    
    printf("new connection!!!\n");
    clen = sizeof(struct sockaddr_in);
    if ((sd = accept(fd, (struct sockaddr *)serv, &clen)) < 0) {
        fprintf(stderr, "failed to accept new relay connection!\n");
        return;
    }
    
    if ((conv = (struct conversation *)malloc(sizeof(struct conversation))) == NULL) {
        fprintf(stderr, "unable to create new connectin from relay!\n");
        return;
    }
    memset(conv, 0, sizeof(struct conversation));
    conv->fd = sd;

    TAILQ_INSERT_TAIL(&conversations, conv, entry);
    srv_add_input(srvctx, conv->fd, conv, message_from_relay);
    srv_add_exceptor(srvctx, srvfail);
    
    if (read(conv->fd, (char *)&netlen, sizeof(uint32_t)) < 0) {
        fprintf(stderr, "unable to read message from relay!\n");
        goto fail;
    }
    netlen = ntohl(netlen);
    if (netlen > sizeof(buf)) {
        fprintf(stderr, "overflow attack by relay/client! Not gonna read in %d bytes\n",
                netlen);
        goto fail;
    }

    framesize = 0;
    while (netlen) {
        if ((rlen = read(conv->fd, (buf + framesize), netlen)) < 1) {
            fprintf(stderr, "unable to read message from relay!\n");
            goto fail;
        }
        framesize += rlen;
        netlen -= rlen;
    }
    if (buf[0] != PUB_ACTION_VENDOR) {
        fprintf(stderr, "first message is not a vendor-specific action frame...dropping!\n");
        goto fail;
    }
    
    printf("read %d bytes frame from relay\n", framesize);
    dpp = (dpp_action_frame *)&buf[1];
    switch (dpp->frame_type) {
        case PKEX_SUB_EXCH_REQ:
            printf("PKEX request...\n");
            if ((conv->handle = pkex_create_peer(DPP_VERSION)) < 1) {
                fprintf(stderr, "can't create pkex instance!\n");
                goto fail;
            }
            if (process_pkex_frame(&buf[1], framesize - 1, conv->handle) < 0) {
                fprintf(stderr, "error processing PKEX frame from relay!\n");
                goto fail;
            }
            break;
        case DPP_SUB_AUTH_REQUEST:
            printf("DPP auth request...\n");
            /*
             * responder and non-mutual...
             */
            if ((conv->handle = dpp_create_peer(NULL, 0, 0, 0)) < 1) {
                goto fail;
            }
            if (process_dpp_auth_frame((unsigned char *)dpp, framesize-1, conv->handle) < 0) {
                fprintf(stderr, "error processing DPP auth frame from relay!\n");
                goto fail;
            }
            break;
        case DPP_CHIRP:
            /*
             * see if we know about this guy...
             */
            printf("DPP chirp!\n");
            if ((rhash = find_tlv(RESPONDER_BOOT_HASH, dpp->attributes, framesize - 1)) == NULL) {
                goto fail;
            }
            if ((fp = fopen(bootstrapfile, "r")) == NULL) {
                goto fail;
            }
            if ((mdctx = EVP_MD_CTX_new()) == NULL) {
                goto fail;
            }
            while (!feof(fp)) {
                memset(pkey, 0, sizeof(pkey));
                if (fscanf(fp, "%d %d %d %s %s", &ret, &opclass, &channel, mac, pkey) < 0) {
                    continue;
                }
                if ((asn1len = EVP_DecodeBlock(keyasn1, (unsigned char *)pkey, strlen(pkey))) < 0) {
                    continue;
                }
                printf("checking %d...", ret);
                EVP_DigestInit(mdctx, EVP_sha256());
                EVP_DigestUpdate(mdctx, "chirp", strlen("chirp"));
                EVP_DigestUpdate(mdctx, keyasn1, asn1len - 1);
                EVP_DigestFinal(mdctx, keyhash, &mdlen);
//                print_buffer("decoded", keyasn1, asn1len);
//                print_buffer("from chirp", TLV_value(rhash), SHA256_DIGEST_LENGTH);
//                print_buffer("computed", keyhash, SHA256_DIGEST_LENGTH);
                if (memcmp(keyhash, TLV_value(rhash), SHA256_DIGEST_LENGTH) == 0) {
                    printf("YES!!!\n");
                    /* 
                     * if so, initiator and try mutual (responder decides anyway)
                     */
                    if ((conv->handle = dpp_create_peer((unsigned char *)&pkey[0], 1, 1, 0)) < 1) {
                        fclose(fp);
                        goto fail;
                    }
                    found = 1;
                    break;
                } else {
                    printf("no\n");
                }
            }
            fclose(fp);
            if (!found) {
                goto fail;
            }
            break;
        default:
            fprintf(stderr, "first message from relay not a DPP/PKEX request!\n");
            goto fail;
    }
    conv->t0 = srv_add_timeout(srvctx, SRV_SEC(10), delete_conv, conv);
    if (0) {
fail:
        close(sd);
        if (conv->handle > 0) {
            dpp_free_peer(conv->handle);

        }
        srv_rem_input(srvctx, sd);
        TAILQ_REMOVE(&conversations, conv, entry);
        free(conv);
    }
    if (mdctx != NULL) {
        EVP_MD_CTX_free(mdctx);
    }
    
    return;
}

int
change_channel (unsigned char *blah, unsigned char foo, unsigned char bar)
{
    return 1;
}

int change_dpp_channel (dpp_handle handle, unsigned char foo, unsigned char bar)
{
    return 1;
}

int change_dpp_freq (dpp_handle handle, unsigned char foo)
{
    return 1;
}

/*
 * takes the IP address of a relay and an index into the bootstrapping key file.
 * Initiates DPP, through the relay, to peer whose DPP URI is at "keyidx".
 */
int
bootstrap_peer (char *relay, int keyidx, int is_initiator, int mauth)
{
    FILE *fp;
    struct conversation *conv;
    struct sockaddr_in clnt;
    int n, opclass, channel;
    unsigned char peermac[ETH_ALEN]; 
    unsigned char keyb64[1024];
    char buf[80], mac[20], *ptr;
    uint32_t netlen;
    struct wired_control ctrl;

    printf("looking for bootstrap key index %d\n", keyidx);
    if ((fp = fopen(bootstrapfile, "r")) == NULL) {
        return -1;
    }
    while (!feof(fp)) {
        memset(keyb64, 0, sizeof(keyb64));
        fscanf(fp, "%d %d %d %s %s", &n, &opclass, &channel, mac, keyb64);
        if (n == keyidx) {
            break;
        }
    }
    if (feof(fp)) {
        fprintf(stderr, "unable to find bootstrap key with index %d\n", keyidx);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    printf("peer is on operating class %d and channel %d, checking...\n", opclass, channel);
    printf("peer's bootstrapping key is %s\n", keyb64);

    ptr = &mac[10];
    sscanf(ptr, "%hhx", &peermac[5]); mac[10] = '\0';
    ptr = &mac[8];
    sscanf(ptr, "%hhx", &peermac[4]); mac[8] = '\0';
    ptr = &mac[6];
    sscanf(ptr, "%hhx", &peermac[3]); mac[6] = '\0';
    ptr = &mac[4];
    sscanf(ptr, "%hhx", &peermac[2]); mac[4] = '\0';
    ptr = &mac[2];
    sscanf(ptr, "%hhx", &peermac[1]); mac[2] = '\0';
    ptr = &mac[0];
    sscanf(ptr, "%hhx", &peermac[0]); 

    if ((conv = (struct conversation *)malloc(sizeof(struct conversation))) == NULL) {
        fprintf(stderr, "unable to create new connectin from relay!\n");
        return -1;
    }
    memset(conv, 0, sizeof(struct conversation));

    printf("connecting to relay at %s\n", relay);
    memset((char *)&clnt, 0, sizeof(struct sockaddr_in));
    clnt.sin_family = AF_INET;
    clnt.sin_addr.s_addr = inet_addr(relay);
    clnt.sin_port = htons(DPP_PORT);
    if ((conv->fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "unable to create socket!\n");
        free(conv);
        return -1;
    }
    if (connect(conv->fd, (struct sockaddr *)&clnt, sizeof(struct sockaddr_in)) < 0) {
        fprintf(stderr, "unable to connect to relay at %s\n", relay);
        close(conv->fd);
        free(conv);
        return -1;
    }
    TAILQ_INSERT_TAIL(&conversations, conv, entry);
    srv_add_input(srvctx, conv->fd, conv, message_from_relay);

    /*
     * information we want to send
     */
    memcpy(ctrl.peermac, peermac, ETH_ALEN);
    ctrl.opclass = opclass;
    ctrl.channel = channel;

    /*
     * construct the message to send
     */
    memset(buf, 0, sizeof(buf));
    ptr = buf;
    netlen = htonl(sizeof(struct wired_control) + 1);
    memcpy(ptr, (char *)&netlen, sizeof(uint32_t));
    ptr += sizeof(uint32_t);

    *ptr = -1;  /* control message, not a valid action frame field */
    ptr++;
    memcpy(ptr, (char *)&ctrl, sizeof(struct wired_control));
    if (write(conv->fd, buf, sizeof(struct wired_control) + sizeof(uint32_t) + 1) < 1) {
        fprintf(stderr, "unable to write message to relay at %s\n", relay);
        goto fail;
    }
    if ((conv->handle = dpp_create_peer(keyb64, is_initiator, mauth, 0)) < 1) {
        goto fail;
    }

    if (0) {
fail:
        if (conv->handle > 0) {
            dpp_free_peer(conv->handle);
        }
        srv_rem_timeout(srvctx, conv->fd);
        close(conv->fd);
        TAILQ_REMOVE(&conversations, conv, entry);
        free(conv);
        return -1;
    }
    conv->t0 = srv_add_timeout(srvctx, SRV_SEC(10), delete_conv, conv);
       
    return 1;
}

void
term (unsigned short reason)
{
    /*
     * not used by controller
     */
    return;
}

int
main (int argc, char **argv)
{
    int c, debug = 0, is_initiator = 0, config_or_enroll = 0, mutual = 1, do_pkex = 0, do_dpp = 1;
    int keyidx = 0, opt, infd, newgroup = 0;
    struct sockaddr_in serv;
    char relay[20], password[80], keyfile[80], signkeyfile[80], enrollee_role[10];
    char *ptr, *endptr, identifier[80], pkexinfo[80];
    unsigned char targetmac[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    if ((srvctx = srv_create_context()) == NULL) {
        fprintf(stderr, "%s: cannot create service context!\n", argv[0]);
        exit(1);
    }
    TAILQ_INIT(&conversations);
    memset(bootstrapfile, 0, 80);
    memset(signkeyfile, 0, 80);
    memset(identifier, 0, 80);
    memset(pkexinfo, 0, 80);
//    strcpy(bootstrapfile, "none");
//    strcpy(signkeyfile, "none");
//    strcpy(identifier, "none");
//    strcpy(pkexinfo, "none");
    for (;;) {
        c = getopt(argc, argv, "hirm:k:I:B:x:b:yae:c:d:p:n:z:");
        if (c < 0) {
            break;
        }
        switch (c) {
            case 'I':           /*  */
                strcpy(relay, optarg);
                break;
            case 'B':           /* bootstrap key file */
                strcpy(bootstrapfile, optarg);
                break;
            case 'k':           /* keyfile */
                strcpy(keyfile, optarg);
                break;
            case 'p':           /* password */
                strcpy(password, optarg);
                do_pkex = 1;
                break;
            case 'n':           /* pkex identifier */
                strcpy(identifier, optarg);
                break;
            case 'd':           /* debug */
                debug = atoi(optarg);
                break;
            case 'i':           /* initiator */
                is_initiator = 1;
                break;
            case 'r':           /* responder */
                is_initiator = 0;
                break;
            case 'b':
                newgroup = atoi(optarg);
                break;
            case 'c':           /* configurator */
                strcpy(signkeyfile, optarg);
                config_or_enroll |= 0x02;
                do_dpp = 1;
                break;
            case 'e':           /* enrollee */
                strcpy(enrollee_role, optarg);
                config_or_enroll |= 0x01;
                do_dpp = 1;
                break;
            case 'a':           /* not mutual authentication */
                mutual = 0;
                do_dpp = 1;
                break;
            case 'm':
                ptr = optarg;
                targetmac[0] = (unsigned char)strtol(ptr, &endptr, 16); ptr = endptr+1; targetmac[0] &= 0xff;
                targetmac[1] = (unsigned char)strtol(ptr, &endptr, 16); ptr = endptr+1; targetmac[1] &= 0xff;
                targetmac[2] = (unsigned char)strtol(ptr, &endptr, 16); ptr = endptr+1; targetmac[2] &= 0xff;
                targetmac[3] = (unsigned char)strtol(ptr, &endptr, 16); ptr = endptr+1; targetmac[3] &= 0xff;
                targetmac[4] = (unsigned char)strtol(ptr, &endptr, 16); ptr = endptr+1; targetmac[4] &= 0xff;
                targetmac[5] = (unsigned char)strtol(ptr, &endptr, 16); ptr = endptr+1; targetmac[5] &= 0xff;
                break;
            case 'y':
                do_pkex = 1;
                do_dpp = 0;
                break;
            case 'x':
                keyidx = atoi(optarg);
                break;
            case 'z':
                strcpy(pkexinfo, optarg);
                break;
            default:
            case 'h':
                fprintf(stderr, 
                        "USAGE: %s [-hCIBapkceirdfgs]\n"
                        "\t-h  show usage, and exit\n"
                        "\t-c <signkey> run DPP as the configurator, sign connectors with <signkey>\n"
                        "\t-e <role> run DPP as the enrollee in the role of <role> (sta or ap)\n"
                        "\t-i  run DPP as the initiator\n"
                        "\t-r  run DPP as the responder\n"
                        "\t-b <curve> Configurator asks for enrollee to generate a new protocol key\n"
                        "\t-a  do not perform mutual authentication in DPP\n"
                        "\t-C <filename> of radio configuration file\n"
                        "\t-I <IP address of relay>\n"
                        "\t-B <filename> of peer bootstrappign keys\n"
                        "\t-p <password> to use for PKEX\n"
                        "\t-z <info> to pass along with public key in PKEX\n"
                        "\t-n <identifier> for the code used in PKEX\n"
                        "\t-k <filename> my bootstrapping key\n"
                        "\t-y  bootstrapping (PKEX) only, don't run DPP\n"
                        "\t-x  <index> DPP only with key <index> in -B <filename>, don't do PKEX\n"
                        "\t-m <MAC address> to initiate to, otherwise uses broadcast\n"
                        "\t-d <debug> set debugging mask\n",
                        argv[0]);
                exit(1);
                
        }
    }
#if 0
    /*
     * if we're doing DPP w/mutual authentication and/or doing PKEX then
     * a bootstrapping key has to be included
     */
    if ((mutual || do_pkex) && (bootstrapfile[0] == 0)) {
        fprintf(stderr, "%s: specify a peer bootstrapping key file with -B <filename>\n", argv[0]);
        exit(1);
    }
    if (is_initiator && !do_pkex && (keyidx == 0)) {
        fprintf(stderr, "%s: either do PKEX or specify an index into bootstrapping file with -x\n",
                argv[0]);
        exit(1);
    }
#else
    if (bootstrapfile[0] == 0) {
        fprintf(stderr, "%s: specify a peer bootstrapping key file with -B <filename>\n", argv[0]);
        exit(1);
    }
#endif
    if ((infd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "%s: unable to create inbound TCP socket!\n", argv[0]);
        exit(1);
    }
    opt = 1;
    if (setsockopt(infd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int)) < 0) {
        fprintf(stderr, "%s: unable to set TCP socket to reuse addr!\n", argv[0]);
        exit(1);
    }
    
    memset((char *)&serv, 0, sizeof(struct sockaddr_in));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = INADDR_ANY;
    serv.sin_port = htons(DPP_PORT);
    if ((bind(infd, (struct sockaddr *)&serv, sizeof(struct sockaddr_in)) < 0) ||
        (listen(infd, 0) < 0)) {
        fprintf(stderr, "%s: unable to bind/listen TCP socket!\n", argv[0]);
        exit(1);
    }
    srv_add_input(srvctx, infd, &serv, new_connection);

    /*
     * initialize data structures...
     */
    if (do_pkex) {
        if (pkex_initialize(is_initiator, password, 
                            identifier[0] == 0 ? NULL : identifier,
                            pkexinfo[0] == 0 ? NULL : pkexinfo, keyfile, debug) < 0) {
            fprintf(stderr, "%s: cannot configure PKEX/DPP, check config file!\n", argv[0]);
            exit(1);
        }
    }
    if (do_dpp) {
        if (dpp_initialize(config_or_enroll, keyfile, signkeyfile[0] == 0 ? NULL : signkeyfile,
                           newgroup, enrollee_role,
                           NULL, 0, NULL, 0, 0, debug) < 0) {
            fprintf(stderr, "%s: cannot configure DPP, check config file!\n", argv[0]);
            exit(1);
        }
    }
    /*
     * TODO: handle initiation, need a MAC address of the target plumbed from CLI,
     * bootstrap the peer with the indicated idx getting a handle, then create new
     * controller-to-relay message to create state for the relay to bind to that MAC
     * address, then begin the conversation using that handle.
     */
    if (keyidx) {
        bootstrap_peer(relay, keyidx, is_initiator, mutual);
    }

    srv_main_loop(srvctx);

    exit(1);
}
