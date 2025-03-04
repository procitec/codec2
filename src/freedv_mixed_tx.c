/*---------------------------------------------------------------------------*\

  FILE........: freedv_mixed_tx.c
  AUTHOR......: Jeroen Vreeken & David Rowe
  DATE CREATED: May 2020

  Demo transmit program for FreeDV API that demonstrates shows mixed
  VHF packet data and speech frames.

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2014 David Rowe

  All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License version 2.1, as
  published by the Free Software Foundation.  This program is
  distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec2.h"
#include "defines.h"
#include "freedv_api.h"

/**********************************************************
        Encoding an ITU callsign (and 4 bit secondary station ID to a valid MAC
   address. http://dmlinking.net/eth_ar.html
 */

// Lookup table for valid callsign characters
static char alnum2code[37] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
                              'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J',
                              'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',
                              'U', 'V', 'W', 'X', 'Y', 'Z', 0};

// Encode a callsign and ssid into a valid MAC address
static int eth_ar_call2mac(uint8_t mac[6], char *callsign, int ssid,
                           bool multicast) {
  uint64_t add = 0;
  int i;

  if (ssid > 15 || ssid < 0) return -1;

  for (i = 7; i >= 0; i--) {
    char c;

    if (i >= strlen(callsign)) {
      c = 0;
    } else {
      c = toupper(callsign[i]);
    }

    int j;

    for (j = 0; j < sizeof(alnum2code); j++) {
      if (alnum2code[j] == c) break;
    }
    if (j == sizeof(alnum2code)) return -1;

    add *= 37;
    add += j;
  }

  mac[0] = ((add >> (40 - 6)) & 0xc0) | (ssid << 2) | 0x02 | multicast;
  mac[1] = (add >> 32) & 0xff;
  mac[2] = (add >> 24) & 0xff;
  mac[3] = (add >> 16) & 0xff;
  mac[4] = (add >> 8) & 0xff;
  mac[5] = add & 0xff;

  return 0;
}

/**********************************************************
        Data channel callback functions
 */

struct my_callback_state {
  int calls;

  unsigned char mac[6];
};

/*
        Called when a packet has been received
        Should not be called in this tx-only test program
 */
void my_datarx(void *callback_state, unsigned char *packet, size_t size) {
  /* This should not happen while sending... */
  fprintf(stderr, "datarx callback called, this should not happen!\n");
}

/*
        Called when a new packet can be send.

        callback_state	Private state variable, not touched by freedv.
        packet		Data array where new packet data is expected
        size		Available size in packet. On return the actual size of
   the packet
 */
void my_datatx(void *callback_state, unsigned char *packet, size_t *size) {
  static int data_type;
  struct my_callback_state *my_cb_state = callback_state;
  my_cb_state->calls++;

  /* Data could come from a network interface, here we just make up some */

  if (data_type % 4 == 1) {
    /*
        Generate a packet with simple test pattern (counting
     */

    /* Send a packet with data */
    int i;

    /* Destination: broadcast */
    memset(packet, 0xff, 6);
    /* Source: our eth_ar encoded callsign+ssid */
    memcpy(packet + 6, my_cb_state->mac, 6);
    /* Ether type: experimental (since this is just a test pattern) */
    packet[12] = 0x01;
    packet[13] = 0x01;

    for (i = 0; i < 64; i++) packet[i + 14] = i;
    *size = i + 14;
  } else if (data_type % 4 == 2) {
    /*
        Generate an FPRS position report
     */

    /* Destination: broadcast */
    memset(packet, 0xff, 6);
    /* Source: our eth_ar encoded callsign+ssid */
    memcpy(packet + 6, my_cb_state->mac, 6);
    /* Ether type: FPRS */
    packet[12] = 0x73;
    packet[13] = 0x70;

    packet[14] = 0x07;  // Position element Lon 86.925026 Lat 27.987850
    packet[15] = 0x3d;  //
    packet[16] = 0xd0;
    packet[17] = 0x37;
    packet[18] = 0xd0 | 0x08 | 0x01;
    packet[19] = 0x3e;
    packet[20] = 0x70;
    packet[21] = 0x85;

    *size = 22;
  } else {
    /*
       Set size to zero, the freedv api will insert a header frame
       This is useful for identifying ourselves
     */
    *size = 0;
  }

  data_type++;
}

/* Determine the amount of 'energy' in the samples by squaring them
   This is not a perfect VAD as noise may trigger it, but works well for
   demonstrations.
 */
static float samples_get_energy(short *samples, int nr) {
  float e = 0;
  int i;

  for (i = 0; i < nr; i++) {
    e += (float)(samples[i] * samples[i]) / (8192);
  }
  e /= nr;

  return e;
}

int main(int argc, char *argv[]) {
  FILE *fin, *fout;
  short *speech_in;
  short *mod_out;
  struct freedv *freedv;
  struct my_callback_state my_cb_state;
  int mode;
  int n_speech_samples;
  int n_nom_modem_samples;
  char *callsign = "NOCALL";
  int ssid = 0;
  bool multicast = false;
  int use_codectx;
  struct CODEC2 *c2;
  int i;
  float data_threshold = 15;

  if (argc < 4) {
    printf(
        "usage: %s 2400A|2400B|800XA InputRawSpeechFile OutputModemRawFile\n"
        " [--codectx]  [--callsign callsign] [--ssid ssid] [--mac-multicast "
        "0|1] [--data-threshold val]\n",
        argv[0]);
    printf("e.g    %s 2400A hts1a.raw hts1a_fdmdv.raw\n", argv[0]);
    exit(1);
  }

  mode = -1;
  if (!strcmp(argv[1], "2400A")) mode = FREEDV_MODE_2400A;
  if (!strcmp(argv[1], "2400B")) mode = FREEDV_MODE_2400B;
  if (!strcmp(argv[1], "800XA")) mode = FREEDV_MODE_800XA;
  if (mode == -1) {
    fprintf(stderr, "Error in mode: %s\n", argv[1]);
    exit(0);
  }

  if (strcmp(argv[2], "-") == 0)
    fin = stdin;
  else if ((fin = fopen(argv[2], "rb")) == NULL) {
    fprintf(stderr, "Error opening input raw speech sample file: %s: %s.\n",
            argv[2], strerror(errno));
    exit(1);
  }

  if (strcmp(argv[3], "-") == 0)
    fout = stdout;
  else if ((fout = fopen(argv[3], "wb")) == NULL) {
    fprintf(stderr, "Error opening output modem sample file: %s: %s.\n",
            argv[3], strerror(errno));
    exit(1);
  }

  use_codectx = 0;

  if (argc > 4) {
    for (i = 4; i < argc; i++) {
      if (strcmp(argv[i], "--codectx") == 0) {
        int c2_mode;

        if ((mode == FREEDV_MODE_700C) || (mode == FREEDV_MODE_700D) ||
            (mode == FREEDV_MODE_800XA)) {
          c2_mode = CODEC2_MODE_700C;
        } else {
          c2_mode = CODEC2_MODE_1300;
        }
        use_codectx = 1;
        c2 = codec2_create(c2_mode);
        assert(c2 != NULL);
      }
      if (strcmp(argv[i], "--callsign") == 0) {
        callsign = argv[i + 1];
      }
      if (strcmp(argv[i], "--ssid") == 0) {
        ssid = atoi(argv[i + 1]);
      }
      if (strcmp(argv[i], "--mac-multicast") == 0) {
        multicast = atoi(argv[i + 1]);
      }
      if (strcmp(argv[i], "--data-threshold") == 0) {
        data_threshold = atof(argv[i + 1]);
      }
    }
  }

  freedv = freedv_open(mode);
  assert(freedv != NULL);

  /* Generate our address */
  eth_ar_call2mac(my_cb_state.mac, callsign, ssid, multicast);

  freedv_set_data_header(freedv, my_cb_state.mac);

  freedv_set_verbose(freedv, 1);

  n_speech_samples = freedv_get_n_speech_samples(freedv);
  n_nom_modem_samples = freedv_get_n_nom_modem_samples(freedv);
  speech_in = (short *)malloc(sizeof(short) * n_speech_samples);
  assert(speech_in != NULL);
  mod_out = (short *)malloc(sizeof(short) * n_nom_modem_samples);
  assert(mod_out != NULL);
  // fprintf(stderr, "n_speech_samples: %d n_nom_modem_samples: %d\n",
  // n_speech_samples, n_nom_modem_samples);

  /* set up callback for data packets */
  freedv_set_callback_data(freedv, my_datarx, my_datatx, &my_cb_state);

  /* OK main loop */

  while (fread(speech_in, sizeof(short), n_speech_samples, fin) ==
         n_speech_samples) {
    if (use_codectx == 0) {
      /* Use the freedv_api to do everything: speech encoding, modulating
       */
      float energy = samples_get_energy(speech_in, n_speech_samples);

      /* Is the audio fragment quiet? */
      if (energy < data_threshold) {
        /* Insert a frame with data instead of speech */
        freedv_datatx(freedv, mod_out);
      } else {
        /* transmit voice frame */
        freedv_tx(freedv, mod_out, speech_in);
      }
    } else {
      /* Use the freedv_api to do the modem part, encode ourselves
         - First encode the frames
         - Get activity from codec2 api
         - Based on activity either send encoded voice or data
       */
      int bits_per_codec_frame = freedv_get_bits_per_codec_frame(freedv);
      int bits_per_modem_frame = freedv_get_bits_per_modem_frame(freedv);
      int bytes_per_codec_frame = (bits_per_codec_frame + 7) / 8;
      int bytes_per_modem_frame = (bits_per_modem_frame + 7) / 8;
      int codec_frames = bits_per_modem_frame / bits_per_codec_frame;
      int samples_per_frame = codec2_samples_per_frame(c2);
      VLA_CALLOC(unsigned char, encoded, bytes_per_codec_frame *codec_frames);
      VLA_CALLOC(unsigned char, rawdata, bytes_per_modem_frame);
      unsigned char *enc_frame = encoded;
      short *speech_frame = speech_in;
      float energy = 0;

      /* Encode the speech ourself (or get it from elsewhere, e.g. network) */
      for (i = 0; i < codec_frames; i++) {
        codec2_encode(c2, enc_frame, speech_frame);
        energy += codec2_get_energy(c2, enc_frame);
        enc_frame += bytes_per_codec_frame;
        speech_frame += samples_per_frame;
      }
      energy /= codec_frames;

      /* Is the audio fragment quiet? */
      if (energy < data_threshold) {
        /* Insert a frame with data instead of speech */
        freedv_datatx(freedv, mod_out);
      } else {
        /* Use the freedv_api to modulate already encoded frames */
        freedv_rawdata_from_codec_frames(freedv, rawdata, encoded);
        freedv_rawdatatx(freedv, mod_out, rawdata);
      }
      VLA_FREE(encoded, rawdata);
    }

    fwrite(mod_out, sizeof(short), n_nom_modem_samples, fout);

    /* if this is in a pipeline, we probably don't want the usual
       buffering to occur */
    if (fout == stdout) fflush(stdout);
  }

  free(speech_in);
  free(mod_out);
  freedv_close(freedv);
  fclose(fin);
  fclose(fout);

  fclose(stdin);
  fclose(stderr);

  return 0;
}
