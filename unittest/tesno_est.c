/*---------------------------------------------------------------------------*\

  FILE........: tesno_est.c
  AUTHORS.....: David Rowe
  DATE CREATED: Mar 2021

  Test for C port of Es/No estimator.

\*---------------------------------------------------------------------------*/

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "defines.h"
#include "ofdm_internal.h"

int main(int argc, char *argv[]) {
  FILE *fin = fopen(argv[1], "rb");
  assert(fin != NULL);
  size_t nsym = atoi(argv[2]);
  assert(nsym >= 0);
  VLA_CALLOC(COMP, rx_sym, nsym);
  size_t nread = fread(rx_sym, sizeof(COMP), nsym, fin);
  assert(nread == nsym);
  fclose(fin);

  float EsNodB = ofdm_esno_est_calc(rx_sym, nsym);
  printf("%f\n", EsNodB);

  VLA_FREE(rx_sym);
  return 0;
}
