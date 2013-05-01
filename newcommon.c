/*
 * Copyright 2001 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by Niels Provos.
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <err.h>
#include <string.h>
#include <setjmp.h>

#include <jpeglib.h>
#include <jerror.h>

#include "newcommon.h"

struct njvirt_barray_control {
  JBLOCKARRAY mem_buffer;       /* => the in-memory buffer */
  JDIMENSION rows_in_array;     /* total virtual array height */
  JDIMENSION blocksperrow;      /* width of array (and of memory buffer) */
  JDIMENSION maxaccess;         /* max rows accessed by access_virt_barray */
  JDIMENSION rows_in_mem;       /* height of memory buffer */
  JDIMENSION rowsperchunk;      /* allocation chunk size in mem_buffer */
  JDIMENSION cur_start_row;     /* first logical row # in the buffer */
  JDIMENSION first_undef_row;   /* row # of first uninitialized row */
  boolean pre_zero;             /* pre-zero mode requested? */
  boolean dirty;                /* do current buffer contents need written? */
  boolean b_s_open;             /* is backing-store data valid? */
  jvirt_barray_ptr next;        /* link to next virtual barray control block */
  void *b_s_info;  /* System-dependent control info */
};

int hib[MAX_COMPS_IN_SCAN], wib[MAX_COMPS_IN_SCAN];
struct jpeg_decompress_struct jinfo;

#define MAX_COMMENTS    10
u_char *comments[MAX_COMMENTS+1];
size_t commentsize[MAX_COMMENTS+1];
int ncomments;
u_int16_t jpg_markers = 0;

struct my_error_mgr {
    struct jpeg_error_mgr pub;    /* "public" fields */

    jmp_buf setjmp_buffer;        /* for return to caller */
};

typedef struct my_error_mgr * my_error_ptr;
void (*stego_eoi_cb)(void *) = NULL;
typedef struct njvirt_barray_control *njvirt_barray_ptr;
njvirt_barray_ptr *dctcoeff;
JBLOCKARRAY dctcompbuf[MAX_COMPS_IN_SCAN];

/*
 * Here's the routine that will replace the standard error_exit method:
 */

METHODDEF(void)
my_error_exit (j_common_ptr cinfo)
{
    my_error_ptr myerr = (my_error_ptr) cinfo->err;

    /* Return control to the setjmp point */
    longjmp(myerr->setjmp_buffer, 1);
}

METHODDEF(void)
my_error_emit (j_common_ptr cinfo, int level)
{
    j_decompress_ptr dinfo = (j_decompress_ptr)cinfo;
    if (cinfo->err->msg_code != JTRC_EOI)
        return;

    if (dinfo->src->bytes_in_buffer == 0) {
        dinfo->src->fill_input_buffer(dinfo);

        /* If we get only two bytes, its a fake EOI from library */
        if (dinfo->src->bytes_in_buffer == 2)
            return;
    }

    /* Give the information to the user */
    (*stego_eoi_cb)(dinfo);
}


u_char
jpeg_getc(j_decompress_ptr cinfo)
{
    struct jpeg_source_mgr *datasrc = cinfo->src;

    if (datasrc->bytes_in_buffer == 0) {
        if (! (*datasrc->fill_input_buffer) (cinfo))
            err(1, "%s: fill_input", __FUNCTION__);
    }
    datasrc->bytes_in_buffer--;
    return GETJOCTET(*datasrc->next_input_byte++);
}

METHODDEF(boolean)
marker_handler(j_decompress_ptr cinfo)
{
    u_int32_t length;
    int offset = cinfo->unread_marker - JPEG_APP0;

    jpg_markers |= 1 << offset;

    length = jpeg_getc(cinfo) << 8;
    length += jpeg_getc(cinfo);
    length -= 2;            /* discount the length word itself */

    while (length-- > 0)
        jpeg_getc(cinfo);

    return (TRUE);
}

METHODDEF(boolean)
comment_handler(j_decompress_ptr cinfo)
{
    u_int32_t length;
    u_char *p;
    
    length = jpeg_getc(cinfo) << 8;
    length += jpeg_getc(cinfo);
    length -= 2;

    p = malloc(length);
    if (p == NULL)
        return (FALSE);

    commentsize[ncomments] = length;
    comments[ncomments++] = p;

    while (length-- > 0) {
        *p++ = jpeg_getc(cinfo);
    }

    return (TRUE);
    
}

void
comments_init(void)
{
    memset(comments, 0, sizeof(comments));
    memset(commentsize, 0, sizeof(commentsize));
    ncomments = 0;
}

void
comments_free(void)
{
    int i;

    for (i = 0; i < ncomments; i++)
        free(comments[i]);
    ncomments = 0;
}



int
jpg_open(char *filename)
{
    char outbuf[1024];
    int i;
    struct my_error_mgr jerr;
    jpeg_component_info *compptr;
    FILE *fin;

    comments_init();
    jpg_markers = 0;
    
    if ((fin = fopen(filename, "r")) == NULL) {
        int error = errno;

        fprintf(stderr, "%s : error: %s\n",
            filename, strerror(error));
        return (-1);
    }

    jinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = my_error_exit;
    if (stego_eoi_cb != NULL)
        jerr.pub.emit_message = my_error_emit;
    /* Establish the setjmp return context for my_error_exit to use. */
    if (setjmp(jerr.setjmp_buffer)) {
        /* Always display the message. */
        (*jinfo.err->format_message) ((j_common_ptr)&jinfo, outbuf);

        fprintf(stderr, "%s : error: %s\n", filename, outbuf);

        /* If we get here, the JPEG code has signaled an error.
         * We need to clean up the JPEG object, close the input file,
         * and return.
         */
        jpeg_destroy_decompress(&jinfo);

        fclose(fin);
        return (-1);
    }
    jpeg_create_decompress(&jinfo);
    jpeg_set_marker_processor(&jinfo, JPEG_COM, comment_handler);
    for (i = 1; i < 16; i++)
        jpeg_set_marker_processor(&jinfo, JPEG_APP0+i, marker_handler);
    jpeg_stdio_src(&jinfo, fin);
    jpeg_read_header(&jinfo, TRUE);

    /* jinfo.quantize_colors = TRUE; */
    dctcoeff = (njvirt_barray_ptr *)jpeg_read_coefficients(&jinfo);

    fclose(fin);

    if (dctcoeff == NULL) {
        fprintf(stderr, "%s : error: can not get coefficients\n",
            filename);
        goto out;
    }

    if (jinfo.out_color_space != JCS_RGB) {
        fprintf(stderr, "%s : error: is not a RGB image\n", filename);
        goto out;
    }
 
    i = jinfo.num_components;
    if (i != 3) {
        fprintf(stderr,
            "%s : error: wrong number of color components: %d\n",
            filename, i);
        goto out;
    }
    
    for(i = 0; i < 3; i++) {
        compptr = jinfo.cur_comp_info[i];
        /*
        fprintf(stderr,
            "input_iMCU_row: %d, v_samp_factor: %d\n",
            jinfo.input_iMCU_row * compptr->v_samp_factor,
            (JDIMENSION) compptr->v_samp_factor);

        fprintf(stderr, "hib: %d, wib: %d\n",
            jinfo.comp_info[i].height_in_blocks,
            jinfo.comp_info[i].width_in_blocks);
        */

        wib[i] = jinfo.comp_info[i].width_in_blocks;
        hib[i] = jinfo.comp_info[i].height_in_blocks;
        dctcompbuf[i] = dctcoeff[i]->mem_buffer;
    }

    return (0);
out:
    jpg_destroy();

    return (-1);
}

void
jpg_finish(void)
{
    jpeg_finish_decompress(&jinfo);
    comments_free();
}

void
jpg_destroy(void)
{
    jpeg_destroy_decompress(&jinfo);
    comments_free();
}