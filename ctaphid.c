#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctaphid.h"
#include "ctap.h"
#include "u2f.h"
#include "time.h"
#include "util.h"

typedef enum
{
    IDLE = 0,
    HANDLING_REQUEST,
} CTAP_STATE;

typedef enum
{
    EMPTY = 0,
    BUFFERING,
    BUFFERED,
} CTAP_BUFFER_STATE;


typedef struct
{
    uint8_t cmd;
    uint32_t cid;
    uint16_t bcnt;
    int offset;
    int bytes_written;
    uint8_t seq;
    uint8_t buf[HID_MESSAGE_SIZE];
} CTAPHID_WRITE_BUFFER;

struct CID
{
    uint32_t cid;
    uint64_t last_used;
    uint8_t busy;
    uint8_t last_cmd;
};


#define SUCESS          0
#define SEQUENCE_ERROR  1

static int state;
static struct CID CIDS[10];
#define CID_MAX (sizeof(CIDS)/sizeof(struct CID))

static uint64_t active_cid_timestamp;

static uint8_t ctap_buffer[CTAPHID_BUFFER_SIZE];
static int ctap_buffer_cmd;
static uint16_t ctap_buffer_bcnt;
static int ctap_buffer_offset;
static int ctap_packet_seq;

static void buffer_reset();

#define CTAPHID_WRITE_INIT      0x01
#define CTAPHID_WRITE_FLUSH     0x02
#define CTAPHID_WRITE_RESET     0x04

#define     ctaphid_write_buffer_init(x)    memset(x,0,sizeof(CTAPHID_WRITE_BUFFER))
static void ctaphid_write(CTAPHID_WRITE_BUFFER * wb, void * _data, int len);

void ctaphid_init()
{
    state = IDLE;
    buffer_reset();
    ctap_reset_state();
}

static uint32_t get_new_cid()
{
    static uint32_t cid = 1;
    do
    {
        cid++;
    }while(cid == 0 || cid == 0xffffffff);
    return cid;
}

static int8_t add_cid(uint32_t cid)
{
    int i;
    for(i = 0; i < CID_MAX-1; i++)
    {
        if (!CIDS[i].busy)
        {
            CIDS[i].cid = cid;
            CIDS[i].busy = 1;
            CIDS[i].last_used = millis();
            return 0;
        }
    }
    return -1;
}

static int8_t cid_exists(uint32_t cid)
{
    int i;
    for(i = 0; i < CID_MAX-1; i++)
    {
        if (CIDS[i].cid == cid)
        {
            return 1;
        }
    }
    return 0;
}

static int8_t cid_refresh(uint32_t cid)
{
    int i;
    for(i = 0; i < CID_MAX-1; i++)
    {
        if (CIDS[i].cid == cid)
        {
            CIDS[i].last_used = millis();
            return 0;
        }
    }
    return -1;
}

static int is_broadcast(CTAPHID_PACKET * pkt)
{
    return (pkt->cid == CTAPHID_BROADCAST_CID);
}

static int is_init_pkt(CTAPHID_PACKET * pkt)
{
    return (pkt->pkt.init.cmd == CTAPHID_INIT);
}

static int is_cont_pkt(CTAPHID_PACKET * pkt)
{
    return !(pkt->pkt.init.cmd & TYPE_INIT);
}


static int is_timed_out()
{
    return (millis() - active_cid_timestamp > 500);
}

static int buffer_packet(CTAPHID_PACKET * pkt)
{
    if (pkt->pkt.init.cmd & TYPE_INIT)
    {
        ctap_buffer_bcnt = ctaphid_packet_len(pkt);
        int pkt_len = (ctap_buffer_bcnt < CTAPHID_INIT_PAYLOAD_SIZE) ? ctap_buffer_bcnt : CTAPHID_INIT_PAYLOAD_SIZE;
        ctap_buffer_cmd = pkt->pkt.init.cmd;
        ctap_buffer_offset = pkt_len;
        ctap_packet_seq = -1;
        memmove(ctap_buffer, pkt->pkt.init.payload, pkt_len);
    }
    else
    {
        int leftover = ctap_buffer_bcnt - ctap_buffer_offset;
        int diff = leftover - CTAPHID_CONT_PAYLOAD_SIZE;
        ctap_packet_seq++;
        if (ctap_packet_seq != pkt->pkt.cont.seq)
        {
            return SEQUENCE_ERROR;
        }

        if (diff <= 0)
        {
            // only move the leftover amount
            memmove(ctap_buffer + ctap_buffer_offset, pkt->pkt.cont.payload, leftover);
            ctap_buffer_offset += leftover;
        }
        else
        {
            memmove(ctap_buffer + ctap_buffer_offset, pkt->pkt.cont.payload, CTAPHID_CONT_PAYLOAD_SIZE);
            ctap_buffer_offset += CTAPHID_CONT_PAYLOAD_SIZE;
        }
    }
    return SUCESS;
}

static void buffer_reset()
{
    ctap_buffer_bcnt = 0;
    ctap_buffer_offset = 0;
    ctap_packet_seq = 0;
}

static int buffer_status()
{
    if (ctap_buffer_bcnt == 0)
    {
        return EMPTY;
    }
    else if (ctap_buffer_offset == ctap_buffer_bcnt)
    {
        return BUFFERED;
    }
    else
    {
        return BUFFERING;
    }
}

static int buffer_cmd()
{
    return ctap_buffer_cmd;
}

static int buffer_len()
{
    return ctap_buffer_bcnt;
}

// Buffer data and send in HID_MESSAGE_SIZE chunks
// if len == 0, FLUSH
static void ctaphid_write(CTAPHID_WRITE_BUFFER * wb, void * _data, int len)
{
    uint8_t * data = (uint8_t *)_data;
    if (_data == NULL)
    {
        if (wb->offset == 0 && wb->bytes_written == 0)
        {
            memmove(wb->buf, &wb->cid, 4);
            wb->offset += 4;

            wb->buf[4] = wb->cmd;
            wb->buf[5] = (wb->bcnt & 0xff00) >> 8;
            wb->buf[6] = (wb->bcnt & 0xff) >> 0;
            wb->offset += 3;
        }

        if (wb->offset > 0)
        {
            memset(wb->buf + wb->offset, 0, HID_MESSAGE_SIZE - wb->offset);
            ctaphid_write_block(wb->buf);
        }
        return;
    }
    int i;
    for (i = 0; i < len; i++)
    {
        if (wb->offset == 0 )
        {
            memmove(wb->buf, &wb->cid, 4);
            wb->offset += 4;

            if (wb->bytes_written == 0)
            {
                wb->buf[4] = wb->cmd;
                wb->buf[5] = (wb->bcnt & 0xff00) >> 8;
                wb->buf[6] = (wb->bcnt & 0xff) >> 0;
                wb->offset += 3;
            }
            else
            {
                wb->buf[4] = wb->seq++;
                wb->offset += 1;
            }
        }
        wb->buf[wb->offset++] = data[i];
        wb->bytes_written += 1;
        if (wb->offset == HID_MESSAGE_SIZE)
        {
            ctaphid_write_block(wb->buf);
            wb->offset = 0;
        }
    }
}


static void ctaphid_send_error(uint32_t cid, uint8_t error)
{
    uint8_t buf[HID_MESSAGE_SIZE];
    CTAPHID_WRITE_BUFFER wb;
    ctaphid_write_buffer_init(&wb);

    wb.cid = cid;
    wb.cmd = CTAPHID_ERROR;
    wb.bcnt = 1;

    ctaphid_write(&wb, &error, 1);
    ctaphid_write(&wb, NULL, 0);
}

static void send_init_response(uint32_t oldcid, uint32_t newcid, uint8_t * nonce)
{
    CTAPHID_INIT_RESPONSE init_resp;
    CTAPHID_WRITE_BUFFER wb;
    ctaphid_write_buffer_init(&wb);
    wb.cid = oldcid;
    wb.cmd = CTAPHID_INIT;
    wb.bcnt = 17;

    memmove(init_resp.nonce, nonce, 8);
    init_resp.cid = newcid;
    init_resp.protocol_version = 0;//?
    init_resp.version_major = 0;//?
    init_resp.version_minor = 0;//?
    init_resp.build_version = 0;//?
    init_resp.capabilities = CTAP_CAPABILITIES;

    ctaphid_write(&wb,&init_resp,sizeof(CTAPHID_INIT_RESPONSE));
    ctaphid_write(&wb,NULL,0);
}


void u2f_hid_check_timeouts()
{
    uint8_t i;
    for(i = 0; i < CID_MAX; i++)
    {
        if (CIDS[i].busy && ((millis() - CIDS[i].last_used) >= 750))
        {
            printf("TIMEOUT CID: %08x\n", CIDS[i].cid);
            ctaphid_send_error(CIDS[i].cid, CTAP1_ERR_TIMEOUT);
            memset(CIDS + i, 0, sizeof(struct CID));
        }
    }

}


void ctaphid_handle_packet(uint8_t * pkt_raw)
{
    CTAPHID_PACKET * pkt = (CTAPHID_PACKET *)(pkt_raw);

    printf("Recv packet\n");
    printf("  CID: %08x \n", pkt->cid);
    printf("  cmd: %02x\n", pkt->pkt.init.cmd);
    if (!is_cont_pkt(pkt)) printf("  length: %d\n", ctaphid_packet_len(pkt));

    int ret;
    uint8_t status;
    uint32_t oldcid;
    uint32_t newcid;
    static CTAPHID_WRITE_BUFFER wb;
    uint32_t active_cid;

    CTAP_RESPONSE ctap_resp;


    if (is_init_pkt(pkt))
    {
        if (ctaphid_packet_len(pkt) != 8)
        {
            printf("Error,invalid length field for init packet\n");
            ctaphid_send_error(pkt->cid, CTAP1_ERR_INVALID_LENGTH);
            return;
        }
        if (pkt->cid == 0)
        {
            printf("Error, invalid cid 0\n");
            ctaphid_send_error(pkt->cid, CTAP1_ERR_INVALID_PARAMETER);
            return;
        }

        ctaphid_init();
        if (is_broadcast(pkt))
        {
            // Check if any existing cids are busy first ?
            printf("adding a new cid\n");
            oldcid = CTAPHID_BROADCAST_CID;
            newcid = get_new_cid();
            ret = add_cid(newcid);
            // handle init here
        }
        else
        {
            printf("synchronizing to cid\n");
            oldcid = pkt->cid;
            newcid = pkt->cid;
            if (cid_exists(newcid))
                ret = cid_refresh(newcid);
            else
                ret = add_cid(newcid);
        }
        if (ret == -1)
        {
            printf("Error, not enough memory for new CID.  return BUSY.\n");
            ctaphid_send_error(pkt->cid, CTAP1_ERR_CHANNEL_BUSY);
            return;
        }
        send_init_response(oldcid, newcid, pkt->pkt.init.payload);
        return;
    }
    else
    {
        // Check if matches existing CID
        if (cid_exists(pkt->cid))
        {
            if (! is_cont_pkt(pkt))
            {
                if (ctaphid_packet_len(pkt) > CTAPHID_BUFFER_SIZE)
                {
                    ctaphid_send_error(pkt->cid, CTAP1_ERR_INVALID_LENGTH);
                    return;
                }
            }
            if (buffer_packet(pkt) == SEQUENCE_ERROR)
            {
                printf("Buffering sequence error\n");
                ctaphid_send_error(pkt->cid, CTAP1_ERR_INVALID_SEQ);
                return;
            }
            ret = cid_refresh(pkt->cid);
            if (ret != 0)
            {
                printf("Error, refresh cid failed\n");
                exit(1);
            }
            active_cid = pkt->cid;
        }
        else if (is_cont_pkt(pkt))
        {
            printf("ignoring unwarranted cont packet\n");
            // Ignore
            return;
        }
        else
        {
            printf("ignoring unwarranted init packet\n");
            // Ignore
            return;
        }
    }



    switch(buffer_status())
    {
        case BUFFERING:
            printf("BUFFERING\n");
            active_cid_timestamp = millis();
            break;

        case EMPTY:
            printf("empty buffer!\n");
        case BUFFERED:
            switch(buffer_cmd())
            {
                case CTAPHID_INIT:
                    printf("CTAPHID_INIT, error this should already be handled\n");
                    exit(1);
                    break;
                case CTAPHID_PING:
                    printf("CTAPHID_PING\n");

                    ctaphid_write_buffer_init(&wb);
                    wb.cid = active_cid;
                    wb.cmd = CTAPHID_PING;
                    wb.bcnt = buffer_len();

                    ctaphid_write(&wb, ctap_buffer, buffer_len());
                    ctaphid_write(&wb, NULL,0);

                    break;

                case CTAPHID_WINK:
                    printf("CTAPHID_WINK\n");

                    if (buffer_len() != 0)
                    {
                        printf("Error,invalid length field for wink packet\n");
                        ctaphid_send_error(pkt->cid, CTAP1_ERR_INVALID_LENGTH);
                        return;
                    }

                    ctaphid_write_buffer_init(&wb);

                    wb.cid = active_cid;
                    wb.cmd = CTAPHID_WINK;

                    ctaphid_write(&wb,NULL,0);

                    break;

                case CTAPHID_CBOR:
                    printf("CTAPHID_CBOR\n");
                    if (buffer_len() == 0)
                    {
                        printf("Error,invalid 0 length field for cbor packet\n");
                        ctaphid_send_error(pkt->cid, CTAP1_ERR_INVALID_LENGTH);
                        return;
                    }

                    ctap_response_init(&ctap_resp);
                    status = ctap_handle_packet(ctap_buffer, buffer_len(), &ctap_resp);

                    ctaphid_write_buffer_init(&wb);
                    wb.cid = active_cid;
                    wb.cmd = CTAPHID_CBOR;
                    wb.bcnt = (ctap_resp.length+1);

                    ctaphid_write(&wb, &status, 1);
                    ctaphid_write(&wb, ctap_resp.data, ctap_resp.length);
                    ctaphid_write(&wb, NULL, 0);
                    break;

                case CTAPHID_MSG:
                    printf("CTAPHID_MSG\n");
                    if (buffer_len() == 0)
                    {
                        printf("Error,invalid 0 length field for MSG/U2F packet\n");
                        ctaphid_send_error(pkt->cid, CTAP1_ERR_INVALID_LENGTH);
                        return;
                    }

                    ctap_response_init(&ctap_resp);
                    u2f_request((struct u2f_request_apdu*)ctap_buffer, &ctap_resp);

                    ctaphid_write_buffer_init(&wb);
                    wb.cid = active_cid;
                    wb.cmd = CTAPHID_MSG;
                    wb.bcnt = (ctap_resp.length);

                    ctaphid_write(&wb, ctap_resp.data, ctap_resp.length);
                    ctaphid_write(&wb, NULL, 0);
                    break;

                default:
                    printf("error, unimplemented HID cmd: %02x\r\n", buffer_cmd());
                    ctaphid_send_error(pkt->cid, CTAP1_ERR_INVALID_COMMAND);
                    break;
            }

            buffer_reset();
            break;

        default:
            printf("invalid buffer state; abort\n");
            exit(1);
            break;
    }
    
    printf("\n");

}

