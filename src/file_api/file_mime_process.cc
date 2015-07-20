//--------------------------------------------------------------------------
// Copyright (C) 2014-2015 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2012-2013 Sourcefire, Inc.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------
/*
**  Author(s):  Hui Cao <huica@cisco.com>
**
**  NOTES
**  9.25.2012 - Initial Source Code. Hui Cao
*/

#include "file_mime_process.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "snort_types.h"
#include "file_api.h"
#include "snort_bounds.h"
#include "util.h"
#include "search_engines/search_tool.h"
#include "protocols/packet.h"
#include "detection_util.h"
#include "framework/data_bus.h"
#include "file_mime_config.h"

typedef struct _MimeToken
{
    const char* name;
    int name_len;
    int search_id;
} MimeToken;

typedef enum _MimeHdrEnum
{
    HDR_CONTENT_TYPE = 0,
    HDR_CONT_TRANS_ENC,
    HDR_CONT_DISP,
    HDR_LAST
} MimeHdrEnum;

const MimeToken mime_hdrs[] =
{
    { "Content-type:", 13, HDR_CONTENT_TYPE },
    { "Content-Transfer-Encoding:", 26, HDR_CONT_TRANS_ENC },
    { "Content-Disposition:", 20, HDR_CONT_DISP },
    { NULL,             0, 0 }
};

typedef struct _MIMESearch
{
    const char* name;
    int name_len;
} MIMESearch;

typedef struct _MIMESearchInfo
{
    int id;
    int index;
    int length;
} MIMESearchInfo;

MIMESearchInfo mime_search_info;

SearchTool* mime_hdr_search_mpse = nullptr;
MIMESearch mime_hdr_search[HDR_LAST];
MIMESearch* mime_current_search = NULL;
static const char* boundary_str = "boundary=";

static void set_mime_buffers(MimeSession* ssn)
{
    if ((ssn != NULL) && (ssn->decode_state == NULL))
    {
        DecodeConfig* conf = ssn->decode_conf;

        ssn->decode_state = NewEmailDecodeState(
            conf->get_max_depth(), conf->get_b64_depth(), conf->get_qp_depth(),
            conf->get_uu_depth(), conf->get_bitenc_depth(),
            conf->get_file_depth());
    }
}

void get_mime_eol(const uint8_t* ptr, const uint8_t* end,
    const uint8_t** eol, const uint8_t** eolm)
{
    const uint8_t* tmp_eol;
    const uint8_t* tmp_eolm;

    assert(eol and eolm);

    if ( !ptr or !end )
    {
        *eol = *eolm = end;
        return;
    }

    tmp_eol = (uint8_t*)memchr(ptr, '\n', end - ptr);
    if (tmp_eol == NULL)
    {
        tmp_eol = end;
        tmp_eolm = end;
    }
    else
    {
        /* end of line marker (eolm) should point to marker and
         * end of line (eol) should point to end of marker */
        if ((tmp_eol > ptr) && (*(tmp_eol - 1) == '\r'))
        {
            tmp_eolm = tmp_eol - 1;
        }
        else
        {
            tmp_eolm = tmp_eol;
        }

        /* move past newline */
        tmp_eol++;
    }

    *eol = tmp_eol;
    *eolm = tmp_eolm;
}

/*
 * Callback function for string search
 *
 * @param   id      id in array of search strings from mime_config.cmds
 * @param   index   index in array of search strings from mime_config.cmds
 * @param   data    buffer passed in to search function
 *
 * @return response
 * @retval 1        commands caller to stop searching
 */
static int search_str_found(void* id, void*, int index, void*, void*)
{
    int search_id = (int)(uintptr_t)id;

    mime_search_info.id = search_id;
    mime_search_info.index = index;
    mime_search_info.length = mime_current_search[search_id].name_len;

    /* Returning non-zero stops search, which is okay since we only look for one at a time */
    return 1;
}

static inline void process_decode_type(const char* start, int length, bool cnt_xf,
    MimeSession* mime_ssn)
{
    const char* tmp = NULL;
    Email_DecodeState* decode_state = (Email_DecodeState*)(mime_ssn->decode_state);

    if (cnt_xf)
    {
        if (decode_state->b64_state.encode_depth > -1)
        {
            tmp = SnortStrcasestr(start, length, "base64");
            if ( tmp != NULL )
            {
                decode_state->decode_type = DECODE_B64;
                return;
            }
        }

        if (decode_state->qp_state.encode_depth > -1)
        {
            tmp = SnortStrcasestr(start, length, "quoted-printable");
            if ( tmp != NULL )
            {
                decode_state->decode_type = DECODE_QP;
                return;
            }
        }

        if (decode_state->uu_state.encode_depth > -1)
        {
            tmp = SnortStrcasestr(start, length, "uuencode");
            if ( tmp != NULL )
            {
                decode_state->decode_type = DECODE_UU;
                return;
            }
        }
    }

    if (decode_state->bitenc_state.depth > -1)
    {
        decode_state->decode_type = DECODE_BITENC;
        return;
    }
}

static inline void setup_decode(const char* data, int size, bool cnt_xf, MimeSession* mime_ssn)
{
    /* Check for Encoding Type */
    if ( mime_ssn->decode_conf && mime_ssn->decode_conf->is_decoding_enabled())
    {
        set_mime_buffers(mime_ssn);
        if (mime_ssn->decode_state != NULL)
        {
            ResetBytesRead((Email_DecodeState*)(mime_ssn->decode_state));
            process_decode_type(data, size, cnt_xf, mime_ssn);
            mime_ssn->state_flags |= MIME_FLAG_EMAIL_ATTACH;
            /* check to see if there are other attachments in this packet */
            if ( ((Email_DecodeState*)(mime_ssn->decode_state))->decoded_bytes )
                mime_ssn->state_flags |= MIME_FLAG_MULTIPLE_EMAIL_ATTACH;
        }
    }
}

/*
 * Handle Headers - Data or Mime
 *
 * @param   packet  standard Packet structure
 *
 * @param   i       index into p->payload buffer to start looking at data
 *
 * @return  i       index into p->payload where we stopped looking at data
 */
static const uint8_t* process_mime_header(
    const uint8_t* ptr,
    const uint8_t* data_end_marker, MimeSession* mime_ssn)
{
    const uint8_t* eol = data_end_marker;
    const uint8_t* eolm = eol;
    const uint8_t* colon;
    const uint8_t* content_type_ptr = NULL;
    const uint8_t* cont_trans_enc = NULL;
    const uint8_t* cont_disp = NULL;
    int header_found;
    const uint8_t* start_hdr;

    start_hdr = ptr;

    /* if we got a content-type in a previous packet and are
     * folding, the boundary still needs to be checked for */
    if (mime_ssn->state_flags & MIME_FLAG_IN_CONTENT_TYPE)
        content_type_ptr = ptr;

    if (mime_ssn->state_flags & MIME_FLAG_IN_CONT_TRANS_ENC)
        cont_trans_enc = ptr;

    if (mime_ssn->state_flags & MIME_FLAG_IN_CONT_DISP)
        cont_disp = ptr;

    while (ptr < data_end_marker)
    {
        int header_name_len;
        int max_header_name_len = 0;
        get_mime_eol(ptr, data_end_marker, &eol, &eolm);

        /* got a line with only end of line marker should signify end of header */
        if (eolm == ptr)
        {
            /* reset global header state values */
            mime_ssn->state_flags &=
                ~(MIME_FLAG_FOLDING | MIME_FLAG_IN_CONTENT_TYPE | MIME_FLAG_DATA_HEADER_CONT
                | MIME_FLAG_IN_CONT_TRANS_ENC );

            mime_ssn->data_state = STATE_DATA_BODY;

            /* if no headers, treat as data */
            if (ptr == start_hdr)
                return eolm;
            else
                return eol;
        }

        /* if we're not folding, see if we should interpret line as a data line
         * instead of a header line */
        if (!(mime_ssn->state_flags & (MIME_FLAG_FOLDING | MIME_FLAG_DATA_HEADER_CONT)))
        {
            char got_non_printable_in_header_name = 0;

            /* if we're not folding and the first char is a space or
             * colon, it's not a header */
            if (isspace((int)*ptr) || *ptr == ':')
            {
                mime_ssn->data_state = STATE_DATA_BODY;
                return ptr;
            }

            /* look for header field colon - if we're not folding then we need
             * to find a header which will be all printables (except colon)
             * followed by a colon */
            colon = ptr;
            while ((colon < eolm) && (*colon != ':'))
            {
                if (((int)*colon < 33) || ((int)*colon > 126))
                    got_non_printable_in_header_name = 1;

                colon++;
            }

            /* Check for Exim 4.32 exploit where number of chars before colon is greater than 64 */
            header_name_len = colon - ptr;
            if ((mime_ssn->data_state != STATE_DATA_UNKNOWN) &&
                (colon < eolm) && (header_name_len > MAX_HEADER_NAME_LEN))
            {
                max_header_name_len = header_name_len;
            }

            /* If the end on line marker and end of line are the same, assume
             * header was truncated, so stay in data header state */
            if ((eolm != eol) &&
                ((colon == eolm) || got_non_printable_in_header_name))
            {
                /* no colon or got spaces in header name (won't be interpreted as a header)
                 * assume we're in the body */
                mime_ssn->state_flags &=
                    ~(MIME_FLAG_FOLDING | MIME_FLAG_IN_CONTENT_TYPE | MIME_FLAG_DATA_HEADER_CONT
                    |MIME_FLAG_IN_CONT_TRANS_ENC);

                mime_ssn->data_state = STATE_DATA_BODY;

                return ptr;
            }

            if (tolower((int)*ptr) == 'c')
            {
                mime_current_search = &mime_hdr_search[0];
                header_found = mime_hdr_search_mpse->find(
                    (const char*)ptr, eolm - ptr, search_str_found, true);

                /* Headers must start at beginning of line */
                if ((header_found > 0) && (mime_search_info.index == 0))
                {
                    switch (mime_search_info.id)
                    {
                    case HDR_CONTENT_TYPE:
                        content_type_ptr = ptr + mime_search_info.length;
                        mime_ssn->state_flags |= MIME_FLAG_IN_CONTENT_TYPE;
                        break;
                    case HDR_CONT_TRANS_ENC:
                        cont_trans_enc = ptr + mime_search_info.length;
                        mime_ssn->state_flags |= MIME_FLAG_IN_CONT_TRANS_ENC;
                        break;
                    case HDR_CONT_DISP:
                        cont_disp = ptr + mime_search_info.length;
                        mime_ssn->state_flags |= MIME_FLAG_IN_CONT_DISP;
                        break;
                    default:
                        break;
                    }
                }
            }
            else if (tolower((int)*ptr) == 'e')
            {
                if ((eolm - ptr) >= 9)
                {
                    if (strncasecmp((const char*)ptr, "Encoding:", 9) == 0)
                    {
                        cont_trans_enc = ptr + 9;
                        mime_ssn->state_flags |= MIME_FLAG_IN_CONT_TRANS_ENC;
                    }
                }
            }
        }
        else
        {
            mime_ssn->state_flags &= ~MIME_FLAG_DATA_HEADER_CONT;
        }

        if (mime_ssn->methods && mime_ssn->methods->handle_header_line)
        {
            int ret = mime_ssn->methods->handle_header_line(mime_ssn->config, ptr, eol, max_header_name_len,
                mime_ssn);
            if (ret < 0)
                return NULL;
            else if (ret > 0)
            {
                /* assume we guessed wrong and are in the body */
                mime_ssn->data_state = STATE_DATA_BODY;
                mime_ssn->state_flags &=
                    ~(MIME_FLAG_FOLDING | MIME_FLAG_IN_CONTENT_TYPE | MIME_FLAG_DATA_HEADER_CONT
                    | MIME_FLAG_IN_CONT_TRANS_ENC | MIME_FLAG_IN_CONT_DISP);
                return ptr;
            }
        }

        /* check for folding
         * if char on next line is a space and not \n or \r\n, we are folding */
        if ((eol < data_end_marker) && isspace((int)eol[0]) && (eol[0] != '\n'))
        {
            if ((eol < (data_end_marker - 1)) && (eol[0] != '\r') && (eol[1] != '\n'))
            {
                mime_ssn->state_flags |= MIME_FLAG_FOLDING;
            }
            else
            {
                mime_ssn->state_flags &= ~MIME_FLAG_FOLDING;
            }
        }
        else if (eol != eolm)
        {
            mime_ssn->state_flags &= ~MIME_FLAG_FOLDING;
        }

        /* check if we're in a content-type header and not folding. if so we have the whole
         * header line/lines for content-type - see if we got a multipart with boundary
         * we don't check each folded line, but wait until we have the complete header
         * because boundary=BOUNDARY can be split across mulitple folded lines before
         * or after the '=' */
        if ((mime_ssn->state_flags &
            (MIME_FLAG_IN_CONTENT_TYPE | MIME_FLAG_FOLDING)) == MIME_FLAG_IN_CONTENT_TYPE)
        {
            if ((mime_ssn->data_state == STATE_MIME_HEADER) && !(mime_ssn->state_flags &
                MIME_FLAG_EMAIL_ATTACH))
            {
                setup_decode((const char*)content_type_ptr, (eolm - content_type_ptr), false,
                    mime_ssn);
            }

            mime_ssn->state_flags &= ~MIME_FLAG_IN_CONTENT_TYPE;
            content_type_ptr = NULL;
        }
        else if ((mime_ssn->state_flags &
            (MIME_FLAG_IN_CONT_TRANS_ENC | MIME_FLAG_FOLDING)) == MIME_FLAG_IN_CONT_TRANS_ENC)
        {
            setup_decode((const char*)cont_trans_enc, (eolm - cont_trans_enc), true, mime_ssn);

            mime_ssn->state_flags &= ~MIME_FLAG_IN_CONT_TRANS_ENC;

            cont_trans_enc = NULL;
        }
        else if (((mime_ssn->state_flags &
            (MIME_FLAG_IN_CONT_DISP | MIME_FLAG_FOLDING)) == MIME_FLAG_IN_CONT_DISP) && cont_disp)
        {
            bool disp_cont = (mime_ssn->state_flags & MIME_FLAG_IN_CONT_DISP_CONT) ? true : false;
            if (mime_ssn->log_config->log_filename && mime_ssn->log_state )
            {
                if (!log_file_name(cont_disp, eolm - cont_disp,
                    &(mime_ssn->log_state->file_log), &disp_cont) )
                    mime_ssn->log_flags |= MIME_FLAG_FILENAME_PRESENT;
            }
            if (disp_cont)
            {
                mime_ssn->state_flags |= MIME_FLAG_IN_CONT_DISP_CONT;
            }
            else
            {
                mime_ssn->state_flags &= ~MIME_FLAG_IN_CONT_DISP;
                mime_ssn->state_flags &= ~MIME_FLAG_IN_CONT_DISP_CONT;
            }

            cont_disp = NULL;
        }

        /* if state was unknown, at this point assume we know */
        if (mime_ssn->data_state == STATE_DATA_UNKNOWN)
            mime_ssn->data_state = STATE_DATA_HEADER;

        ptr = eol;

        if (ptr == data_end_marker)
            mime_ssn->state_flags |= MIME_FLAG_DATA_HEADER_CONT;
    }

    return ptr;
}

/* Get the end of data body (excluding boundary)*/
static const uint8_t* GetDataEnd(const uint8_t* data_start,
    const uint8_t* data_end_marker)
{
    /* '\r\n' + '--' + MIME boundary string */
    const int Max_Search = 4 + MAX_MIME_BOUNDARY_LEN;
    uint8_t* start;
    /*Exclude 2 bytes because either \r\n or '--'  at the end */
    uint8_t* end = (uint8_t*)data_end_marker - 2;

    /*Search for the start of boundary, should be less than boundary length*/
    if (end > data_start + Max_Search)
        start = end - Max_Search;
    else
        start = (uint8_t*)data_start;

    while (end > start)
    {
        if (*(--end) != '\n')
            continue;

        if ((*(end+1) == '-') && (*(end+2) == '-'))
        {
            if ((end > start) && (*(end-1) == '\r'))
                return (end - 1);
            else
                return end;
        }
        break;
    }
    return data_end_marker;
}

/*
 * Handle DATA_BODY state
 * @param   packet standard Packet structure
 * @param   i index into p->payload buffer to start looking at data
 * @return  i index into p->payload where we stopped looking at data
 */
static const uint8_t* process_mime_body(const uint8_t* ptr,
    const uint8_t* data_end, MimeSession* mime_ssn, bool is_data_end)
{
    Email_DecodeState* decode_state = (Email_DecodeState*)(mime_ssn->decode_state);

    if (mime_ssn->state_flags & MIME_FLAG_EMAIL_ATTACH)
    {
        const uint8_t* attach_start = ptr;
        const uint8_t* attach_end;

        if (is_data_end )
        {
            attach_end = GetDataEnd(ptr, data_end);
        }
        else
        {
            attach_end = data_end;
        }

        if ( attach_start < attach_end )
        {
            if (EmailDecode(attach_start, attach_end, decode_state) < DECODE_SUCCESS )
            {
                if (mime_ssn->methods && mime_ssn->methods->decode_alert)
                    mime_ssn->methods->decode_alert(mime_ssn->decode_state);
            }
        }
    }

    if (is_data_end)
    {
        mime_ssn->data_state = STATE_MIME_HEADER;
        mime_ssn->state_flags &= ~MIME_FLAG_EMAIL_ATTACH;
    }

    return data_end;
}

/*
 * Reset MIME session state
 */
void MimeSession::reset_mime_state()
{
    mime_ssn->data_state = STATE_DATA_INIT;
    mime_ssn->state_flags = 0;
    ClearEmailDecodeState(decode_state);
}

/*
 * Main function for mime processing
 *
 * This should be called when mime data is available
 */
const uint8_t* MimeSession::process_mime_data_paf(Flow* flow, const uint8_t* start, const uint8_t* end,
    MimeSession* mime_ssn, bool upload, FilePosition position)
{
    bool done_data = false;

    if (mime_ssn->methods && mime_ssn->methods->is_end_of_data)
    {
        done_data = mime_ssn->methods->is_end_of_data(flow);
    }

    /* if we've just entered the data state, check for a dot + end of line
     * if found, no data */
    if ((mime_ssn->data_state == STATE_DATA_INIT) ||
        (mime_ssn->data_state == STATE_DATA_UNKNOWN))
    {
        if ((start < end) && (*start == '.'))
        {
            const uint8_t* eol = NULL;
            const uint8_t* eolm = NULL;

            get_mime_eol(start, end, &eol, &eolm);

            /* this means we got a real end of line and not just end of payload
             * and that the dot is only char on line */
            if ((eolm != end) && (eolm == (start + 1)))
            {
                /* if we're normalizing and not ignoring data copy data end marker
                 * and dot to alt buffer */
                if (mime_ssn->methods && mime_ssn->methods->normalize_data)
                {
                    if (mime_ssn->methods->normalize_data(mime_ssn->config, start, end) < 0)
                        return NULL;
                }

                reset_mime_state(mime_ssn);

                return eol;
            }
        }

        if (mime_ssn->data_state == STATE_DATA_INIT)
            mime_ssn->data_state = STATE_DATA_HEADER;

        /* XXX A line starting with a '.' that isn't followed by a '.' is
         * deleted (RFC 821 - 4.5.2.  TRANSPARENCY).  If data starts with
         * '. text', i.e a dot followed by white space then text, some
         * servers consider it data header and some data body.
         * Postfix and Qmail will consider the start of data:
         * . text\r\n
         * .  text\r\n
         * to be part of the header and the effect will be that of a
         * folded line with the '.' deleted.  Exchange will put the same
         * in the body which seems more reasonable. */
    }

    // FIXIT-L why is this being set?  we don't search file data until
    // we set it again below after decoding.  can it be deleted?
    if ( mime_ssn->decode_conf && (!mime_ssn->decode_conf->is_ignore_data()))
        set_file_data((uint8_t*)start, (end - start));

    if ((mime_ssn->data_state == STATE_DATA_HEADER) ||
        (mime_ssn->data_state == STATE_DATA_UNKNOWN))
    {
#ifdef DEBUG_MSGS
        if (mime_ssn->data_state == STATE_DATA_HEADER)
        {
            DEBUG_WRAP(DebugMessage(DEBUG_FILE, "DATA HEADER STATE ~~~~~~~~~~~~~~~~~~~~~~\n"); );
        }
        else
        {
            DEBUG_WRAP(DebugMessage(DEBUG_FILE, "DATA UNKNOWN STATE ~~~~~~~~~~~~~~~~~~~~~\n"); );
        }
#endif

        start = process_mime_header(start, end, mime_ssn);
        if (start == NULL)
            return NULL;
    }

    if (mime_ssn->methods && mime_ssn->methods->normalize_data)
    {
        if (mime_ssn->methods->normalize_data(mime_ssn->config, start, end) < 0)
            return NULL;
    }
    /* now we shouldn't have to worry about copying any data to the alt buffer
     *      * only mime headers if we find them and only if we're ignoring data */

    while ((start != NULL) && (start < end))
    {
        switch (mime_ssn->data_state)
        {
        case STATE_MIME_HEADER:
            DEBUG_WRAP(DebugMessage(DEBUG_FILE, "MIME HEADER STATE ~~~~~~~~~~~~~~~~~~~~~~\n"); );
            start = process_mime_header(start, end, mime_ssn);
            break;
        case STATE_DATA_BODY:
            DEBUG_WRAP(DebugMessage(DEBUG_FILE, "DATA BODY STATE ~~~~~~~~~~~~~~~~~~~~~~~~\n"); );
            start = process_mime_body(start, end, mime_ssn, isFileEnd(position) );
            break;
        }
    }

    /* We have either reached the end of MIME header or end of MIME encoded data*/

    if ((mime_ssn->decode_state) != NULL)
    {
        DecodeConfig* conf= mime_ssn->decode_conf;
        Email_DecodeState* ds = (Email_DecodeState*)(mime_ssn->decode_state);

        if (conf)
        {
            int detection_size = getDetectionSize(conf->get_b64_depth(),
                conf->get_qp_depth(), conf->get_uu_depth(),
                conf->get_bitenc_depth(), ds);
            set_file_data(ds->decodePtr, (uint16_t)detection_size);
        }

        /*Process file type/file signature*/
        if (file_api->file_process(flow, (uint8_t*)ds->decodePtr,
            (uint16_t)ds->decoded_bytes, position, upload, false)
            && (isFileStart(position))&& mime_ssn->log_state)
        {
            set_file_name_from_log(&(mime_ssn->log_state->file_log), flow);
        }
        ResetDecodedBytes((Email_DecodeState*)(mime_ssn->decode_state));
    }

    /* if we got the data end reset state, otherwise we're probably still in the data
     *      * to expect more data in next packet */
    if (done_data)
    {
        reset_mime_state(mime_ssn);
        if (mime_ssn->methods && mime_ssn->methods->reset_state)
            mime_ssn->methods->reset_state(flow);
    }

    return end;
}

/*
 *  * Main function for mime processing
 *   *
 *    * This should be called when mime data is available
 *     */
const uint8_t* process_mime_data(Flow* flow, const uint8_t* start,
    const uint8_t* data_end_marker, MimeSession* mime_ssn, bool upload, FilePosition position)
{
    const uint8_t* attach_start = start;
    const uint8_t* attach_end;

    if (position != SNORT_FILE_POSITION_UNKNOWN)
    {
        //FilePosition position = file_api->get_file_position(p);
        process_mime_data_paf(flow, attach_start, data_end_marker,
            mime_ssn, upload, position);
        return data_end_marker;
    }

    initFilePosition(&position, file_api->get_file_processed_size(flow));
    /* look for boundary */
    while (start < data_end_marker)
    {
        /*Found the boundary, start processing data*/
        if (process_mime_paf_data(&(mime_ssn->mime_boundary),  *start))
        {
            attach_end = start;
            finalFilePosition(&position);
            process_mime_data_paf(flow, attach_start, attach_end,
                mime_ssn, upload, position);
            position = SNORT_FILE_START;
            attach_start = start + 1;
        }

        start++;
    }

    if ((start == data_end_marker) && (attach_start < data_end_marker))
    {
        updateFilePosition(&position, file_api->get_file_processed_size(flow));
        process_mime_data_paf(flow, attach_start, data_end_marker,
            mime_ssn, upload, position);
    }

    return data_end_marker;
}

/*
 * This is the initialization function for mime processing.
 * This should be called when snort initializes
 */
void MimeSession::init(void)
{
    const MimeToken* tmp;

    /* Header search */
    mime_hdr_search_mpse = new SearchTool();
    if (mime_hdr_search_mpse == NULL)
    {
        // FIXIT-M make configurable or at least fall back to any
        // available search engine
        FatalError("Could not instantiate ac_bnfa search engine.\n");
    }

    for (tmp = &mime_hdrs[0]; tmp->name != NULL; tmp++)
    {
        mime_hdr_search[tmp->search_id].name = tmp->name;
        mime_hdr_search[tmp->search_id].name_len = tmp->name_len;

        mime_hdr_search_mpse->add(tmp->name, tmp->name_len, tmp->search_id);
    }

    mime_hdr_search_mpse->prep();
}

// Free anything that needs it before shutting down preprocessor
void MimeSession::close(void)
{
    if (mime_hdr_search_mpse != NULL)
        delete mime_hdr_search_mpse;
}

MimeSession::MimeSession(DecodeConfig* dconf, MailLogConfig* mconf)
{

}

MimeSession::~MimeSession()
{
    if ( decode_state )
        free(decode_state);

    if ( log_state )
        free(log_state);
}
