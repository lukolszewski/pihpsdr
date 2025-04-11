/* Copyright (C)
* 2019 - John Melton, G0ORX/N6LYT
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include <SoapySDR/Constants.h>
#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Version.h>
#include <SoapySDR/Logger.h>

#include <wdsp.h>   // only needed for the resampler

#include "audio.h"
#include "band.h"
#include "channel.h"
#include "discovered.h"
#include "ext.h"
#include "filter.h"
#include "main.h"
#include "message.h"
#include "mode.h"
#include "radio.h"
#include "receiver.h"
#include "soapy_protocol.h"
#include "transmitter.h"
#include "vfo.h"

#define MAX_CHANNELS 2
static SoapySDRStream *rx_stream[MAX_CHANNELS];
static SoapySDRStream *tx_stream;
static SoapySDRDevice *soapy_device;
static int max_samples;

static double rx_bandwidth = 12000000.0;
static double tx_bandwidth = 2000000.0;

static GThread *receive_thread_id = NULL;
static gpointer receive_thread(gpointer data);
static gpointer receive_dual_thread(gpointer data);
static gpointer receive_dupli_thread(gpointer data);

static gboolean running;

static int mic_samples = 0;
static int mic_sample_divisor = 1;

double lo_frequency = 100e6;

static int max_tx_samples;
static float *output_buffer;
static int output_buffer_index;

// cppcheck-suppress unusedFunction
SoapySDRDevice *get_soapy_device() {
  return soapy_device;
}

Band rx_bands[] = {
    //{430e6, 440e6, 4.282000e+08},     // 70cm
    {430e6, 440e6, 435e6},     // 70cm
    {144e6, 148e6, 142e6},     // 2m
    {1240e6, 1300e6, 1238e6},  // 23cm
    {2300e6, 2450e6, 2298e6}   // 13cm
};

Band tx_bands[] = { // Different LO frequencies due to wanting to use narrower filters.
    //{430e6, 440e6, 4.282000e+08},     // 70cm
    {430e6, 431e6, 429.9e6},     // 70cm
    {431e6, 432e6, 430.9e6},     // 70cm
    {432e6, 433e6, 431.9e6},     // 70cm
    {433e6, 434e6, 432.9e6},     // 70cm
    {434e6, 435e6, 433.9e6},     // 70cm
    {435e6, 436e6, 434.9e6},     // 70cm
    {436e6, 437e6, 435.9e6},     // 70cm
    {437e6, 438e6, 436.9e6},     // 70cm
    {438e6, 439e6, 437.9e6},     // 70cm
    {439e6, 440e6, 440.1e6},     // 70cm
    {144e6, 148e6, 142e6},     // 2m
    {1240e6, 1300e6, 1238e6},  // 23cm
    {2300e6, 2450e6, 2298e6}   // 13cm
};

double get_lo_frequency(double freq_hz) {
    int num_bands = sizeof(rx_bands) / sizeof(rx_bands[0]);
    for (int i = 0; i < num_bands; i++) {
        if (freq_hz >= rx_bands[i].lower_bound && freq_hz <= rx_bands[i].upper_bound) {
            t_print("Get LO called returning %e",rx_bands[i].lo_frequency);
            return rx_bands[i].lo_frequency;
        }
    }
    t_print("Get LO called returning default %e, received %e",freq_hz - 2e6,freq_hz);
    return freq_hz - 2e6;  // default: 2 MHz lower
}

double get_tx_lo_frequency(double freq_hz) {
    int num_bands = sizeof(tx_bands) / sizeof(tx_bands[0]);
    for (int i = 0; i < num_bands; i++) {
        if (freq_hz >= tx_bands[i].lower_bound && freq_hz <= tx_bands[i].upper_bound) {
            t_print("Get TX LO called returning %e",tx_bands[i].lo_frequency);
            return tx_bands[i].lo_frequency;
        }
    }
    t_print("Get LO called returning default %e, received %e",freq_hz - 2e6,freq_hz);
    return freq_hz - 1e6;  // default: 1 MHz lower
}

void soapy_protocol_set_mic_sample_rate(int rate) {
  mic_sample_divisor = rate / 48000;
}

void soapy_protocol_change_sample_rate(RECEIVER *rx) {
  //
  // rx->mutex already locked, so we can call this  only
  // if the radio is stopped -- we cannot change the resampler
  // while the receive thread is stuck in rx_add_iq_samples()
  //
#if 0
  //
  //  sample code to query, set, and query the sample rate
  //  (without using a resampler). However, going to lower
  //  sample rates should also involve going to smaller
  //  band widths, otherwise there will be aliases
  //
  int rc;
  double d;
  t_print("%s: setting samplerate=%f\n", __FUNCTION__, (double)rx->sample_rate);
  d = SoapySDRDevice_getSampleRate(soapy_device, SOAPY_SDR_RX, rx->adc);
  d = SoapySDRDevice_getBandwidth(soapy_device, SOAPY_SDR_RX, rx->adc);
  rc = SoapySDRDevice_setSampleRate(soapy_device, SOAPY_SDR_RX, rx->adc, (double)rx->sample_rate);

  if (rc != 0) {
    t_print("%s: SoapySDRDevice_setSampleRate(%f) failed: %s\n", __FUNCTION__, (double)rx->sample_rate,
            SoapySDR_errToStr(rc));
  }

  d = SoapySDRDevice_getSampleRate(soapy_device, SOAPY_SDR_RX, rx->adc);
  d = SoapySDRDevice_getBandwidth(soapy_device, SOAPY_SDR_RX, rx->adc);
#endif

  //
  // We stick to the hardware sample rate and use the WDSP resampler
  //
  if (rx->sample_rate == radio_sample_rate) {
    if (rx->resample_buffer != NULL) {
      g_free(rx->resample_buffer);
      rx->resample_buffer = NULL;
      rx->resample_buffer_size = 0;
    }

    if (rx->resampler != NULL) {
      destroy_resample(rx->resampler);
      rx->resampler = NULL;
    }
  } else {
    if (rx->resample_buffer != NULL) {
      g_free(rx->resample_buffer);
      rx->resample_buffer = NULL;
    }

    if (rx->resampler != NULL) {
      destroy_resample(rx->resampler);
      rx->resampler = NULL;
    }

    rx->resample_buffer_size = 2 * max_samples / (radio_sample_rate / rx->sample_rate);
    rx->resample_buffer = g_new(double, rx->resample_buffer_size);
    rx->resampler = create_resample (1, max_samples, rx->buffer, rx->resample_buffer, radio_sample_rate, rx->sample_rate,
                                     0.0, 0, 1.0);
  }
}

void soapy_protocol_create_receiver(RECEIVER *rx) {
  int rc;
  mic_sample_divisor = rx->sample_rate / 48000;
  t_print("%s: device=%p adc=%d setting bandwidth=%f\n", __FUNCTION__, soapy_device, rx->adc, rx_bandwidth);
  rc = SoapySDRDevice_setBandwidth(soapy_device, SOAPY_SDR_RX, rx->adc, rx_bandwidth);

  if (rc != 0) {
    t_print("%s: SoapySDRDevice_setBandwidth(%f) failed: %s\n", __FUNCTION__, (double)rx_bandwidth, SoapySDR_errToStr(rc));
  }

  t_print("%s: setting samplerate=%f device=%p adc=%d mic_sample_divisor=%d\n", __FUNCTION__, (double)radio_sample_rate,
          soapy_device, rx->adc, mic_sample_divisor);

  t_print("Writing RX oversampling setting");
  rc = SoapySDRDevice_writeSetting(soapy_device, "OVERSAMPLING", "32");
  if (rc != 0) {
    t_print("%s: SoapySDRDevice setting oversampling failed: %s\n", __FUNCTION__,
            SoapySDR_errToStr(rc));
  }
  rc = SoapySDRDevice_setSampleRate(soapy_device, SOAPY_SDR_RX, rx->adc, (double)radio_sample_rate);

  if (rc != 0) {
    t_print("%s: SoapySDRDevice_setSampleRate(%f) failed: %s\n", __FUNCTION__, (double)radio_sample_rate,
            SoapySDR_errToStr(rc));
  }

  size_t channel = rx->adc;
#if defined(SOAPY_SDR_API_VERSION) && (SOAPY_SDR_API_VERSION < 0x00080000)
  t_print("%s: SoapySDRDevice_setupStream(version<0x00080000): channel=%ld\n", __FUNCTION__, channel);
  rc = SoapySDRDevice_setupStream(soapy_device, &rx_stream[channel], SOAPY_SDR_RX, SOAPY_SDR_CF32, &channel, 1, NULL);

  if (rc != 0) {
    t_print("%s: SoapySDRDevice_setupStream (RX) failed: %s\n", __FUNCTION__, SoapySDR_errToStr(rc));
    g_idle_add(fatal_error, "Soapy Setup RX Stream Failed");
  }

#else
  t_print("%s: SoapySDRDevice_setupStream(version>=0x00080000): channel=%ld\n", __FUNCTION__, channel);
  rx_stream[channel] = SoapySDRDevice_setupStream(soapy_device, SOAPY_SDR_RX, SOAPY_SDR_CF32, &channel, 1, NULL);

  if (rx_stream[channel] == NULL) {
    t_print("%s: SoapySDRDevice_setupStream (RX) failed (rx_stream is NULL)\n", __FUNCTION__);
    g_idle_add(fatal_error, "Soapy Setup RX Stream Failed");
  }

#endif
  t_print("%s: id=%d soapy_device=%p rx_stream=%p\n", __FUNCTION__, rx->id, soapy_device, rx_stream);
  max_samples = SoapySDRDevice_getStreamMTU(soapy_device, rx_stream[channel]);
  t_print("%s: max_samples=%d\n", __FUNCTION__, max_samples);

  if (max_samples > (2 * rx->fft_size)) {
    max_samples = 2 * rx->fft_size;
  }

  rx->buffer = g_new(double, max_samples * 2);

  if (rx->sample_rate == radio_sample_rate) {
    rx->resample_buffer = NULL;
    rx->resampler = NULL;
    rx->resample_buffer_size = 0;
  } else {
    rx->resample_buffer_size = 2 * max_samples / (radio_sample_rate / rx->sample_rate);
    rx->resample_buffer = g_new(double, rx->resample_buffer_size);
    rx->resampler = create_resample (1, max_samples, rx->buffer, rx->resample_buffer, radio_sample_rate, rx->sample_rate,
                                     0.0, 0, 1.0);
  }

  t_print("%s: max_samples=%d buffer=%p\n", __FUNCTION__, max_samples, rx->buffer);
}

void soapy_protocol_create_receivers(RECEIVER *rx[]) {

    int rc;

    rc = SoapySDRDevice_writeSetting(soapy_device, "OVERSAMPLING", "32");
    if (rc != 0) {
      t_print("%s: SoapySDRDevice setting oversampling failed: %s\n", __FUNCTION__,
              SoapySDR_errToStr(rc));
    }

  for (int i = 0; i < RECEIVERS; i++) {

    mic_sample_divisor = rx[i]->sample_rate / 48000;
    t_print("%s: device=%p adc=%d setting bandwidth=%f\n", __FUNCTION__, soapy_device, rx[i]->adc, rx_bandwidth);
    rc = SoapySDRDevice_setBandwidth(soapy_device, SOAPY_SDR_RX, rx[i]->adc, rx_bandwidth);

    if (rc != 0) {
      t_print("%s: SoapySDRDevice_setBandwidth(%f) failed: %s\n", __FUNCTION__, (double)rx_bandwidth, SoapySDR_errToStr(rc));
    }

    t_print("Writing RX oversampling setting");

    t_print("%s: setting samplerate=%f device=%p adc=%d mic_sample_divisor=%d\n", __FUNCTION__, (double)radio_sample_rate,
            soapy_device, rx[i]->adc, mic_sample_divisor);
    rc = SoapySDRDevice_setSampleRate(soapy_device, SOAPY_SDR_RX, rx[i]->adc, (double)radio_sample_rate);

    if (rc != 0) {
      t_print("%s: SoapySDRDevice_setSampleRate(%f) failed: %s\n", __FUNCTION__, (double)radio_sample_rate,
              SoapySDR_errToStr(rc));
    }
  }
  //size_t channel = rx->adc;
  size_t *channels = malloc(RECEIVERS * sizeof(size_t));
  for (int i = 0; i < RECEIVERS; ++i) {
      channels[i] = i;
  }
  t_print("%s: SoapySDRDevice_setupStream(version>=0x00080000): RECEIVERS=%d\n", __FUNCTION__, RECEIVERS);
  rx_stream[0] = SoapySDRDevice_setupStream(soapy_device, SOAPY_SDR_RX, SOAPY_SDR_CF32, channels, RECEIVERS, NULL);

  if (rx_stream[0] == NULL) {
      t_print("%s: SoapySDRDevice_setupStream (RX) failed (rx_stream is NULL)\n", __FUNCTION__);
      g_idle_add(fatal_error, "Soapy Setup RX Stream Failed");
  }

  free(channels);

  for (int i = 0; i < RECEIVERS; ++i) {
    if (!rx[i]) continue;

    max_samples = SoapySDRDevice_getStreamMTU(soapy_device, rx_stream[0]);

    if (max_samples > (2 * rx[i]->fft_size)) {
      max_samples = 2 * rx[i]->fft_size;
    }

    rx[i]->buffer = g_new(double, max_samples * 2);

    if (rx[i]->sample_rate == radio_sample_rate) {
      rx[i]->resample_buffer = NULL;
      rx[i]->resampler = NULL;
      rx[i]->resample_buffer_size = 0;
    } else {
      rx[i]->resample_buffer_size = 2 * max_samples / (radio_sample_rate / rx[i]->sample_rate);
      rx[i]->resample_buffer = g_new(double, rx[i]->resample_buffer_size);
      rx[i]->resampler = create_resample(1,max_samples,rx[i]->buffer,rx[i]->resample_buffer,radio_sample_rate,rx[i]->sample_rate,0.0,0,1.0
      );
    }

    t_print("%s: id=%d max_samples=%d buffer=%p\n", __FUNCTION__, rx[i]->id, max_samples, rx[i]->buffer);
  }
}

void soapy_protocol_create_duplicate_receivers(RECEIVER *rx[]) {

  int rc;

  rc = SoapySDRDevice_writeSetting(soapy_device, "OVERSAMPLING", "32");
  if (rc != 0) {
    t_print("%s: SoapySDRDevice setting oversampling failed: %s\n", __FUNCTION__,
            SoapySDR_errToStr(rc));
  }

  for (int i = 0; i < RECEIVERS; i++) {
    int rc;
    mic_sample_divisor = rx[i]->sample_rate / 48000;
    t_print("%s: device=%p adc=%d setting bandwidth=%f\n", __FUNCTION__, soapy_device, rx[i]->adc, rx_bandwidth);
    rc = SoapySDRDevice_setBandwidth(soapy_device, SOAPY_SDR_RX, rx[i]->adc, rx_bandwidth);

    if (rc != 0) {
      t_print("%s: SoapySDRDevice_setBandwidth(%f) failed: %s\n", __FUNCTION__, (double)rx_bandwidth, SoapySDR_errToStr(rc));
    }

    t_print("%s: setting samplerate=%f device=%p adc=%d mic_sample_divisor=%d\n", __FUNCTION__, (double)radio_sample_rate,
            soapy_device, rx[i]->adc, mic_sample_divisor);
    rc = SoapySDRDevice_setSampleRate(soapy_device, SOAPY_SDR_RX, rx[i]->adc, (double)radio_sample_rate);

    if (rc != 0) {
      t_print("%s: SoapySDRDevice_setSampleRate(%f) failed: %s\n", __FUNCTION__, (double)radio_sample_rate,
              SoapySDR_errToStr(rc));
    }
  }
  size_t channel = rx[0]->adc;
  t_print("%s: SoapySDRDevice_setupStream(version>=0x00080000): channel=%ld\n", __FUNCTION__, channel);
  rx_stream[channel] = SoapySDRDevice_setupStream(soapy_device, SOAPY_SDR_RX, SOAPY_SDR_CF32, &channel, 1, NULL);

  if (rx_stream[channel] == NULL) {
    t_print("%s: SoapySDRDevice_setupStream (RX) failed (rx_stream is NULL)\n", __FUNCTION__);
    g_idle_add(fatal_error, "Soapy Setup RX Stream Failed");
  }

  for (int i = 0; i < RECEIVERS; ++i) {
    if (!rx[i]) continue;

    max_samples = SoapySDRDevice_getStreamMTU(soapy_device, rx_stream[0]);

    if (max_samples > (2 * rx[i]->fft_size)) {
      max_samples = 2 * rx[i]->fft_size;
    }

    rx[i]->buffer = g_new(double, max_samples * 2);

    if (rx[i]->sample_rate == radio_sample_rate) {
      rx[i]->resample_buffer = NULL;
      rx[i]->resampler = NULL;
      rx[i]->resample_buffer_size = 0;
    } else {
      rx[i]->resample_buffer_size = 2 * max_samples / (radio_sample_rate / rx[i]->sample_rate);
      rx[i]->resample_buffer = g_new(double, rx[i]->resample_buffer_size);
      rx[i]->resampler = create_resample(1,max_samples,rx[i]->buffer,rx[i]->resample_buffer,radio_sample_rate,rx[i]->sample_rate,0.0,0,1.0
      );
    }

    t_print("%s: id=%d max_samples=%d buffer=%p\n", __FUNCTION__, rx[i]->id, max_samples, rx[i]->buffer);
  }
}

void soapy_protocol_start_receiver(RECEIVER *rx) {
  int rc;
  t_print("%s: id=%d soapy_device=%p rx_stream=%p\n", __FUNCTION__, rx->id, soapy_device, rx_stream);
  size_t channel = rx->adc;
  double rate = SoapySDRDevice_getSampleRate(soapy_device, SOAPY_SDR_RX, rx->adc);
  t_print("%s: rate=%f\n", __FUNCTION__, rate);
  t_print("%s: activate Stream\n", __FUNCTION__);
  rc = SoapySDRDevice_activateStream(soapy_device, rx_stream[channel], 0, 0LL, 0);

  if (rc != 0) {
    t_print("%s: SoapySDRDevice_activateStream failed: %s\n", __FUNCTION__, SoapySDR_errToStr(rc));
    g_idle_add(fatal_error, "Soapy Start RX Stream failed");
  }

  t_print("%s: create receiver_thread\n", __FUNCTION__);
  receive_thread_id = g_thread_new( "soapy_rx", receive_thread, rx);
  t_print("%s: receiver_thread_id=%p\n", __FUNCTION__, receive_thread_id);
}

void soapy_protocol_start_receivers(RECEIVER *rx[]) {
  int rc;
  t_print("%s: id1=%d, id2=%d soapy_device=%p rx_stream=%p\n", __FUNCTION__, rx[0]->id,rx[1]->id, soapy_device, rx_stream);

  double rate1 = SoapySDRDevice_getSampleRate(soapy_device, SOAPY_SDR_RX, rx[0]->adc);
  double rate2 = SoapySDRDevice_getSampleRate(soapy_device, SOAPY_SDR_RX, rx[1]->adc);
  t_print("%s: rate1=%f\n", __FUNCTION__, rate1);
  t_print("%s: rate1=%f\n", __FUNCTION__, rate2);
  t_print("%s: activate Stream\n", __FUNCTION__);
  rc = SoapySDRDevice_activateStream(soapy_device, rx_stream[0], 0, 0LL, 0);

  if (rc != 0) {
    t_print("%s: SoapySDRDevice_activateStream failed: %s\n", __FUNCTION__, SoapySDR_errToStr(rc));
    g_idle_add(fatal_error, "Soapy Start RX Stream failed");
  }

  t_print("%s: create receiver_thread\n", __FUNCTION__);
  receive_thread_id = g_thread_new( "soapy_rx", receive_dual_thread, rx);
  t_print("%s: receiver_thread_id=%p\n", __FUNCTION__, receive_thread_id);
}

void soapy_protocol_start_duplicate_receivers(RECEIVER *rx[]) {
  int rc;
  t_print("%s: id1=%d, id2=%d soapy_device=%p rx_stream=%p\n", __FUNCTION__, rx[0]->id,rx[1]->id, soapy_device, rx_stream);

  double rate1 = SoapySDRDevice_getSampleRate(soapy_device, SOAPY_SDR_RX, rx[0]->adc);
  double rate2 = SoapySDRDevice_getSampleRate(soapy_device, SOAPY_SDR_RX, rx[1]->adc);
  t_print("%s: rate1=%f\n", __FUNCTION__, rate1);
  t_print("%s: rate1=%f\n", __FUNCTION__, rate2);
  t_print("%s: activate Stream\n", __FUNCTION__);
  rc = SoapySDRDevice_activateStream(soapy_device, rx_stream[0], 0, 0LL, 0);

  if (rc != 0) {
    t_print("%s: SoapySDRDevice_activateStream failed: %s\n", __FUNCTION__, SoapySDR_errToStr(rc));
    g_idle_add(fatal_error, "Soapy Start RX Stream failed");
  }

  t_print("%s: create receiver_thread\n", __FUNCTION__);
  receive_thread_id = g_thread_new( "soapy_rx", receive_dupli_thread, rx);
  t_print("%s: receiver_thread_id=%p\n", __FUNCTION__, receive_thread_id);
}

void soapy_protocol_create_transmitter(TRANSMITTER *tx) {
  int rc;

  t_print("%s: device=%p adc=%d setting bandwidth=%f\n", __FUNCTION__, soapy_device, tx->dac, tx_bandwidth);
  rc = SoapySDRDevice_setBandwidth(soapy_device, SOAPY_SDR_TX, tx->dac, tx_bandwidth);

  if (rc != 0) {
    t_print("%s: SoapySDRDevice_setBandwidth(%f) failed: %s\n", __FUNCTION__, (double)tx_bandwidth, SoapySDR_errToStr(rc));
  }

  t_print("Writing TX oversampling setting");
  rc = SoapySDRDevice_writeSetting(soapy_device, "OVERSAMPLING", "32");
  if (rc != 0) {
    t_print("%s: SoapySDRDevice setting oversampling failed: %s\n", __FUNCTION__,
            SoapySDR_errToStr(rc)); 
  }

  t_print("%s: setting samplerate=%f\n", __FUNCTION__, (double)tx->iq_output_rate);
  rc = SoapySDRDevice_setSampleRate(soapy_device, SOAPY_SDR_TX, tx->dac, (double)tx->iq_output_rate);

  if (rc != 0) {
    t_print("%s: SoapySDRDevice_setSampleRate(%f) failed: %s\n", __FUNCTION__, (double)tx->iq_output_rate,
            SoapySDR_errToStr(rc));
  }

  size_t channel = tx->dac;
  t_print("%s: SoapySDRDevice_setupStream: channel=%ld\n", __FUNCTION__, channel);
#if defined(SOAPY_SDR_API_VERSION) && (SOAPY_SDR_API_VERSION < 0x00080000)
  rc = SoapySDRDevice_setupStream(soapy_device, &tx_stream, SOAPY_SDR_TX, SOAPY_SDR_CF32, &channel, 1, NULL);

  if (rc != 0) {
    t_print("%s: SoapySDRDevice_setupStream (TX) failed: %s\n", __FUNCTION__, SoapySDR_errToStr(rc));
    g_idle_add(fatal_error, "Soapy Setup TX Stream Failed");
  }

#else
  tx_stream = SoapySDRDevice_setupStream(soapy_device, SOAPY_SDR_TX, SOAPY_SDR_CF32, &channel, 1, NULL);

  if (tx_stream == NULL) {
    t_print("%s: SoapySDRDevice_setupStream (TX) failed: %s\n", __FUNCTION__, SoapySDR_errToStr(rc));
    g_idle_add(fatal_error, "Soapy Setup TX Stream Failed");
  }

#endif
  max_tx_samples = SoapySDRDevice_getStreamMTU(soapy_device, tx_stream);

  if (max_tx_samples > (2 * tx->fft_size)) {
    max_tx_samples = 2 * tx->fft_size;
  }

  t_print("%s: max_tx_samples=%d\n", __FUNCTION__, max_tx_samples);
  output_buffer = (float *)malloc(max_tx_samples * sizeof(float) * 2);
}

void soapy_protocol_start_transmitter(TRANSMITTER *tx) {
  int rc;
  double rate = SoapySDRDevice_getSampleRate(soapy_device, SOAPY_SDR_TX, tx->dac);
  t_print("soapy_protocol_start_transmitter: activateStream rate=%f\n", rate);
  rc = SoapySDRDevice_activateStream(soapy_device, tx_stream, 0, 0LL, 0);

  if (rc != 0) {
    t_print("soapy_protocol_start_transmitter: SoapySDRDevice_activateStream failed: %s\n", SoapySDR_errToStr(rc));
    g_idle_add(fatal_error, "Soapy Start TX Stream Failed");
  }
}

void soapy_protocol_stop_receiver(const RECEIVER *rx) {
  // argument rx unused
  running = FALSE;

  if (receive_thread_id) {
    g_thread_join(receive_thread_id);
    receive_thread_id = NULL;
  }
}

// cppcheck-suppress unusedFunction
void soapy_protocol_stop_transmitter(TRANSMITTER *tx) {
  int rc;
  t_print("soapy_protocol_stop_transmitter: deactivateStream\n");
  // argument tx unused
  rc = SoapySDRDevice_deactivateStream(soapy_device, tx_stream, 0, 0LL);

  if (rc != 0) {
    t_print("soapy_protocol_stop_transmitter: SoapySDRDevice_deactivateStream failed: %s\n", SoapySDR_errToStr(rc));
    g_idle_add(fatal_error, "Soapy Stop TX Stream Failed");
  }
}

void soapy_protocol_init(gboolean hf) {
  SoapySDRKwargs args = {};
  char temp[32];
  SoapySDR_setLogLevel(SOAPY_SDR_TRACE);
  t_print("%s: hf=%d driver=%s\n", __FUNCTION__, hf, radio->name);
  // initialize the radio
  SoapySDRKwargs_set(&args, "driver", radio->name);

  if (strcmp(radio->name, "rtlsdr") == 0) {
    snprintf(temp, 32, "%d", radio->info.soapy.rtlsdr_count);
    SoapySDRKwargs_set(&args, "rtl", temp);

    if (hf) {
      SoapySDRKwargs_set(&args, "direct_samp", "2");
    } else {
      SoapySDRKwargs_set(&args, "direct_samp", "0");
    }
  } else if (strcmp(radio->name, "sdrplay") == 0) {
    snprintf(temp, 32, "SDRplay Dev%d", radio->info.soapy.sdrplay_count);
    t_print("%s: label=%s\n", __FUNCTION__, temp);
    SoapySDRKwargs_set(&args, "label", temp);
  }

  soapy_device = SoapySDRDevice_make(&args);

  if (soapy_device == NULL) {
    t_print("%s: SoapySDRDevice_make failed: %s\n", __FUNCTION__, SoapySDRDevice_lastError());
    g_idle_add(fatal_error, "Soapy Make Device Failed");
  }

 // t_print("Writing initialoversampling setting");
 // int rc = SoapySDRDevice_writeSetting(soapy_device, "OVERSAMPLING", "16");
 // if (rc != 0) {
 //   t_print("%s: SoapySDRDevice setting oversampling failed: %s\n", __FUNCTION__,
 //           SoapySDR_errToStr(rc));
 // }

  SoapySDRKwargs_clear(&args);
  t_print("%s: soapy_device=%p\n", __FUNCTION__, soapy_device);

  if (can_transmit) {
    if (transmitter->local_microphone) {
      if (audio_open_input() != 0) {
        t_print("%s: audio_open_input failed\n", __FUNCTION__);
        transmitter->local_microphone = 0;
      }
    }
  }
}

static void *receive_thread(void *arg) {
  //
  //  Since no mic samples arrive in SOAPY, we must use
  //  the incoming RX samples as a "heart beat" for the
  //  transmitter.
  //
  double isample;
  double qsample;
  int flags = 0;
  long long timeNs = 0;
  long timeoutUs = 100000L;
  int i;
  RECEIVER *rx = (RECEIVER *)arg;
  float *buffer = g_new(float, max_samples * 2);
  void *buffs[] = {buffer};
  float fsample;
  running = TRUE;
  t_print("soapy_protocol: receive_thread\n");
  size_t channel = rx->adc;

  while (running) {
    int elements = SoapySDRDevice_readStream(soapy_device, rx_stream[channel], buffs, max_samples, &flags, &timeNs,
                   timeoutUs);

    //t_print("soapy_protocol_receive_thread: SoapySDRDevice_readStream failed: max_samples=%d read=%d\n",max_samples,elements);
    if (elements < 0) {
      continue;
    }

    for (i = 0; i < elements; i++) {
      rx->buffer[i * 2] = (double)buffer[i * 2];
      rx->buffer[(i * 2) + 1] = (double)buffer[(i * 2) + 1];
    }

    if (rx->resampler != NULL) {
      int samples = xresample(rx->resampler);

      for (i = 0; i < samples; i++) {
        isample = rx->resample_buffer[i * 2];
        qsample = rx->resample_buffer[(i * 2) + 1];

        if (iqswap) {
          rx_add_iq_samples(rx, qsample, isample);
        } else {
          rx_add_iq_samples(rx, isample, qsample);
        }

        if (can_transmit) {
          mic_samples++;

          if (mic_samples >= mic_sample_divisor) { // reduce to 48000
            if (transmitter != NULL) {
              fsample = transmitter->local_microphone ? audio_get_next_mic_sample() : 0.0F;
            } else {
              fsample = 0.0F;
            }

            tx_add_mic_sample(transmitter, fsample);
            mic_samples = 0;
          }
        }
      }
    } else {
      for (i = 0; i < elements; i++) {
        isample = rx->buffer[i * 2];
        qsample = rx->buffer[(i * 2) + 1];

        if (iqswap) {
          rx_add_iq_samples(rx, qsample, isample);
        } else {
          rx_add_iq_samples(rx, isample, qsample);
        }

        if (can_transmit) {
          mic_samples++;

          if (mic_samples >= mic_sample_divisor) { // reduce to 48000
            if (transmitter != NULL) {
              fsample = transmitter->local_microphone ? audio_get_next_mic_sample() : 0.0F;
            } else {
              fsample = 0.0F;
            }

            tx_add_mic_sample(transmitter, fsample);
            mic_samples = 0;
          }
        }
      }
    }
  }

  t_print("soapy_protocol: receive_thread: SoapySDRDevice_deactivateStream\n");
  SoapySDRDevice_deactivateStream(soapy_device, rx_stream[channel], 0, 0LL);
  /*
  t_print("soapy_protocol: receive_thread: SoapySDRDevice_closeStream\n");
  SoapySDRDevice_closeStream(soapy_device,rx_stream[channel]);
  t_print("soapy_protocol: receive_thread: SoapySDRDevice_unmake\n");
  SoapySDRDevice_unmake(soapy_device);
  */
  return NULL;
}

static gpointer receive_dual_thread(gpointer data) {
  RECEIVER **rx = (RECEIVER **)data;
  int flags = 0;
  long long timeNs = 0;
  long timeoutUs = 100000L;
  int i, j;
  float fsample;
  double isample, qsample;

  t_print("soapy_protocol: receive_dual_thread\n");

  float *buffers[RECEIVERS];
  void *buffs[RECEIVERS];

  for (i = 0; i < RECEIVERS; ++i) {
    buffers[i] = g_new(float, max_samples * 2);
    buffs[i] = buffers[i];
  }

  running = TRUE;

  while (running) {
    int elements = SoapySDRDevice_readStream(
      soapy_device, rx_stream[0], buffs, max_samples, &flags, &timeNs, timeoutUs
    );

    if (elements < 0) {
      continue;
    }

    // First copy all the data from the buffers to each receiver
    for (i = 0; i < RECEIVERS; ++i) {
      if (!rx[i]) continue;

      for (j = 0; j < elements; ++j) {
        rx[i]->buffer[j * 2] = (double)buffers[i][j * 2];
        rx[i]->buffer[j * 2 + 1] = (double)buffers[i][j * 2 + 1];
      }
    }

    // Now process each receiver's samples separately without microphone handling
    for (i = 0; i < RECEIVERS; ++i) {
      if (!rx[i]) continue;

      if (rx[i]->resampler != NULL) {
        int samples = xresample(rx[i]->resampler);

        for (j = 0; j < samples; ++j) {
          isample = rx[i]->resample_buffer[j * 2];
          qsample = rx[i]->resample_buffer[j * 2 + 1];

          if (iqswap) {
            rx_add_iq_samples(rx[i], qsample, isample);
          } else {
            rx_add_iq_samples(rx[i], isample, qsample);
          }
        }
      } else {
        for (j = 0; j < elements; ++j) {
          isample = rx[i]->buffer[j * 2];
          qsample = rx[i]->buffer[j * 2 + 1];

          if (iqswap) {
            rx_add_iq_samples(rx[i], qsample, isample);
          } else {
            rx_add_iq_samples(rx[i], isample, qsample);
          }
        }
      }
    }

    // Handle microphone sampling once per batch of SDR samples
    // This ensures consistent microphone timing regardless of number of receivers
    if (can_transmit) {
      // We need to increment mic_samples once for each original IQ sample
      for (j = 0; j < elements; j++) {
        mic_samples++;
        if (mic_samples >= mic_sample_divisor) {
          if (transmitter != NULL) {
            fsample = transmitter->local_microphone ? audio_get_next_mic_sample() : 0.0F;
          } else {
            fsample = 0.0F;
          }
          tx_add_mic_sample(transmitter, fsample);
          mic_samples = 0;
        }
      }
    }
  }

  t_print("soapy_protocol: receive_dual_thread: SoapySDRDevice_deactivateStream\n");
  SoapySDRDevice_deactivateStream(soapy_device, rx_stream[0], 0, 0LL);

  for (i = 0; i < RECEIVERS; ++i) {
    g_free(buffers[i]);
  }

  return NULL;
}

static gpointer receive_dupli_thread(gpointer data) {
  RECEIVER **rx = (RECEIVER **)data;
  int flags = 0;
  long long timeNs = 0;
  long timeoutUs = 100000L;
  int i, j;
  float fsample;
  double isample, qsample;

  t_print("soapy_protocol: receive_dupli_thread\n");

  float *buffer = g_new(float, max_samples * 2);
  void *buffs[] = { buffer };

  running = TRUE;

  while (running) {
    int elements = SoapySDRDevice_readStream(
      soapy_device, rx_stream[0], buffs, max_samples, &flags, &timeNs, timeoutUs
    );

    if (elements < 0) {
      continue;
    }

    // First copy the raw buffer to each receiver's buffer
    for (i = 0; i < elements; ++i) {
      double i_sample = (double)buffer[i * 2];
      double q_sample = (double)buffer[i * 2 + 1];

      for (j = 0; j < RECEIVERS; ++j) {
        if (!rx[j]) continue;
        rx[j]->buffer[i * 2] = i_sample;
        rx[j]->buffer[i * 2 + 1] = q_sample;
      }
    }

    // Process each receiver's samples
    for (j = 0; j < RECEIVERS; ++j) {
      if (!rx[j]) continue;

      if (rx[j]->resampler != NULL) {
        int samples = xresample(rx[j]->resampler);

        for (i = 0; i < samples; ++i) {
          isample = rx[j]->resample_buffer[i * 2];
          qsample = rx[j]->resample_buffer[i * 2 + 1];

          if (iqswap) {
            rx_add_iq_samples(rx[j], qsample, isample);
          } else {
            rx_add_iq_samples(rx[j], isample, qsample);
          }
        }
      } else {
        for (i = 0; i < elements; ++i) {
          isample = rx[j]->buffer[i * 2];
          qsample = rx[j]->buffer[i * 2 + 1];

          if (iqswap) {
            rx_add_iq_samples(rx[j], qsample, isample);
          } else {
            rx_add_iq_samples(rx[j], isample, qsample);
          }
        }
      }
    }

    // Handle microphone sampling once per batch of SDR samples
    // This ensures consistent microphone timing regardless of number of receivers
    if (can_transmit) {
      // We need to increment mic_samples once for each original IQ sample
      for (i = 0; i < elements; i++) {
        mic_samples++;
        if (mic_samples >= mic_sample_divisor) {
          if (transmitter != NULL) {
            fsample = transmitter->local_microphone ? audio_get_next_mic_sample() : 0.0F;
          } else {
            fsample = 0.0F;
          }
          tx_add_mic_sample(transmitter, fsample);
          mic_samples = 0;
        }
      }
    }
  }

  t_print("soapy_protocol: receive_dupli_thread: SoapySDRDevice_deactivateStream\n");
  SoapySDRDevice_deactivateStream(soapy_device, rx_stream[0], 0, 0LL);
  g_free(buffer);
  return NULL;
}

void soapy_protocol_iq_samples(float isample, float qsample) {
  int flags = 0;

  if (radio_is_transmitting()) {
    //
    // The "iqswap" logic has now been removed  from transmitter.c
    // and moved here, because this is where it is also handled
    // upon RX.
    //
    if (iqswap) {
      output_buffer[(output_buffer_index * 2)] = qsample;
      output_buffer[(output_buffer_index * 2) + 1] = isample;
    } else {
      output_buffer[(output_buffer_index * 2)] = isample;
      output_buffer[(output_buffer_index * 2) + 1] = qsample;
    }

    output_buffer_index++;

    if (output_buffer_index >= max_tx_samples) {
      const void *tx_buffs[] = {output_buffer};
      long long timeNs = 0;
      long timeoutUs = 100000L;
      int elements = SoapySDRDevice_writeStream(soapy_device, tx_stream, tx_buffs, max_tx_samples, &flags, timeNs, timeoutUs);

      if (elements != max_tx_samples) {
        t_print("soapy_protocol_iq_samples: writeStream returned %d for %d elements\n", elements, max_tx_samples);
      }

      output_buffer_index = 0;
    }
  }
}

// cppcheck-suppress unusedFunction
void soapy_protocol_stop() {
  t_print("soapy_protocol_stop\n");
  running = FALSE;
}

void printFrequencyComponents(const SoapySDRDevice *device, int direction, size_t channel)
{
    size_t numComponents = 0;
    char **components = SoapySDRDevice_listFrequencies(device, direction, channel, &numComponents);

    for (size_t i = 0; i < numComponents; i++)
    {
        const char *name = components[i];
        double freq = SoapySDRDevice_getFrequencyComponent(device, direction, channel, name);
        t_print("component %s: %e\n", name, (double)freq);

        size_t numRanges = 0;
        SoapySDRRange *ranges = SoapySDRDevice_getFrequencyRangeComponent(device, direction, channel, name, &numRanges);
        for (size_t j = 0; j < numRanges; j++)
        {
            t_print("  range %zu: %e Hz to %e Hz\n", j, (double)ranges[j].minimum, (double)ranges[j].maximum);
        }
    }

    // Free components and ranges if required by SoapySDR implementation
}

void soapy_protocol_set_rx_frequency(RECEIVER *rx, int v) {
  if (soapy_device != NULL) {

    double f = (double)(vfo[v].frequency - vfo[v].lo);

    double new_lo_freq = get_lo_frequency(f);

    SoapySDRKwargs args = {}; // This zero-initializes the structure
    
    // Initialize the structure
    args.size = 0;
    args.keys = NULL;
    args.vals = NULL;

    double offset = f - new_lo_freq;
    t_print("Setting rx no %d, freq  %e with lo %e, and offsett %e",rx->adc,f,new_lo_freq,offset);


    double current_lo_freq = SoapySDRDevice_getFrequencyComponent(soapy_device, SOAPY_SDR_RX, rx->adc, "RF");
    if (current_lo_freq != new_lo_freq) {
      t_print("Setting LO to %e freq first",new_lo_freq);
      int rc = SoapySDRDevice_setFrequencyComponent(soapy_device, SOAPY_SDR_RX, rx->adc, "RF", new_lo_freq, NULL);
      if (rc != 0) {
        t_print("soapy_protocol: SoapySDRDevice_setFrequency(RX) failed: %s\n", SoapySDR_errToStr(rc));
      }
      lo_frequency = new_lo_freq;
    }else{
      t_print("No need to set LO, already where it needs to be.");
    }
    
    int rc = SoapySDRDevice_setFrequencyComponent(soapy_device, SOAPY_SDR_RX, rx->adc, "BB", offset, NULL);
    if (rc != 0) {
      t_print("soapy_protocol: SoapySDRDevice_setFrequency(RX) failed: %s\n", SoapySDR_errToStr(rc));
    }
    printFrequencyComponents(soapy_device, SOAPY_SDR_RX, rx->adc);
    SoapySDRKwargs_clear(&args);
  }
}

void soapy_protocol_set_tx_frequency(TRANSMITTER *tx) {
  int v;
  v = vfo_get_tx_vfo();

  if (can_transmit && soapy_device != NULL) {
    double f;

    if (vfo[v].ctun) {
      f = (double)(vfo[v].ctun_frequency);
    } else {
      f = (double)(vfo[v].frequency);
    }

    if (vfo[v].xit_enabled) {
      f += (double)(vfo[v].xit);
    }
    double new_lo_freq = get_tx_lo_frequency(f);

    SoapySDRKwargs args = {}; // This zero-initializes the structure
    
    // Initialize the structure
    args.size = 0;
    args.keys = NULL;
    args.vals = NULL;

    double offset = f - new_lo_freq;
    t_print("Setting tx id %d,to freq  %e with lo %e, and offsett %e",tx->dac,f,new_lo_freq,offset);

    double current_lo_freq = SoapySDRDevice_getFrequencyComponent(soapy_device, SOAPY_SDR_TX, tx->dac, "RF");
    if (current_lo_freq != new_lo_freq) {
      t_print("Setting LO to %e freq first",new_lo_freq);
      int rc = SoapySDRDevice_setFrequencyComponent(soapy_device, SOAPY_SDR_TX, tx->dac, "RF", new_lo_freq, NULL);
      if (rc != 0) {
        t_print("soapy_protocol: SoapySDRDevice_setFrequency(RX) failed: %s\n", SoapySDR_errToStr(rc));
      }
      //lo_frequency = new_lo_freq;
    }else{
      t_print("No need to set LO, already where it needs to be.");
    }
    
    int rc = SoapySDRDevice_setFrequencyComponent(soapy_device, SOAPY_SDR_TX, tx->dac, "BB", offset, NULL);
    if (rc != 0) {
      t_print("soapy_protocol: SoapySDRDevice_setFrequency(TX) failed: %s\n", SoapySDR_errToStr(rc));
    }
    printFrequencyComponents(soapy_device, SOAPY_SDR_TX, tx->dac);
    SoapySDRKwargs_clear(&args);
  }
}
/*
void soapy_protocol_set_tx_frequency(TRANSMITTER *tx) {
  int v;
  v = vfo_get_tx_vfo();

  if (can_transmit && soapy_device != NULL) {
    double f;

    if (vfo[v].ctun) {
      f = (double)(vfo[v].ctun_frequency);
    } else {
      f = (double)(vfo[v].frequency);
    }

    if (vfo[v].xit_enabled) {
      f += (double)(vfo[v].xit);
    }

    t_print("soapy_protocol_set_tx_frequency: %f\n",f);
        int rc = SoapySDRDevice_setFrequency(soapy_device, SOAPY_SDR_TX, tx->dac, f, NULL);

    if (rc != 0) {
      t_print("soapy_protocol: SoapySDRDevice_setFrequency(TX) failed: %s\n", SoapySDR_errToStr(rc));
    }
    printFrequencyComponents(soapy_device, SOAPY_SDR_TX, tx->dac);
  }
}
*/

void soapy_protocol_set_rx_antenna(RECEIVER *rx, int ant) {
  if (soapy_device != NULL) {
    if (ant >= (int) radio->info.soapy.rx_antennas) { ant = (int) radio->info.soapy.rx_antennas - 1; }

    t_print("soapy_protocol: set_rx_antenna: %s\n", radio->info.soapy.rx_antenna[ant]);
    int rc = SoapySDRDevice_setAntenna(soapy_device, SOAPY_SDR_RX, rx->adc, radio->info.soapy.rx_antenna[ant]);

    if (rc != 0) {
      t_print("soapy_protocol: SoapySDRDevice_setAntenna RX failed: %s\n", SoapySDR_errToStr(rc));
    }
  }
}

void soapy_protocol_set_tx_antenna(TRANSMITTER *tx, int ant) {
  if (soapy_device != NULL) {
    if (ant >= (int) radio->info.soapy.tx_antennas) { ant = (int) radio->info.soapy.tx_antennas - 1; }

    t_print("soapy_protocol: set_tx_antenna: %s\n", radio->info.soapy.tx_antenna[ant]);
    int rc = SoapySDRDevice_setAntenna(soapy_device, SOAPY_SDR_TX, tx->dac, radio->info.soapy.tx_antenna[ant]);

    if (rc != 0) {
      t_print("soapy_protocol: SoapySDRDevice_setAntenna TX failed: %s\n", SoapySDR_errToStr(rc));
    }
  }
}

void soapy_protocol_set_gain(RECEIVER *rx) {
  int rc;
  //t_print("soapy_protocol_set_gain: adc=%d gain=%f\n",gain);
  rc = SoapySDRDevice_setGain(soapy_device, SOAPY_SDR_RX, rx->adc, adc[rx->adc].gain);

  if (rc != 0) {
    t_print("soapy_protocol: SoapySDRDevice_setGain failed: %s\n", SoapySDR_errToStr(rc));
  }
}

void soapy_protocol_attenuate(RECEIVER *rx) {
  int rc;
  //t_print("soapy_protocol_set_gain: adc=%d gain=%f\n",gain);
  rc = SoapySDRDevice_setGain(soapy_device, SOAPY_SDR_RX, rx->adc, adc[rx->adc].min_gain);

  if (rc != 0) {
    t_print("soapy_protocol: SoapySDRDevice_setGain failed: %s\n", SoapySDR_errToStr(rc));
  }
}

void soapy_protocol_unattenuate(RECEIVER *rx) {
  soapy_protocol_set_gain(rx);
}

void soapy_protocol_set_gain_element(const RECEIVER *rx, char *name, int gain) {
  int rc;
  t_print("%s: adc=%d %s=%d\n", __FUNCTION__, rx->adc, name, gain);
  rc = SoapySDRDevice_setGainElement(soapy_device, SOAPY_SDR_RX, rx->adc, name, (double)gain);

  if (rc != 0) {
    t_print("%s: SoapySDRDevice_setGainElement %s failed: %s\n", __FUNCTION__, name, SoapySDR_errToStr(rc));
  }
}

void soapy_protocol_set_tx_gain(TRANSMITTER *tx, int gain) {
  int rc;
  rc = SoapySDRDevice_setGain(soapy_device, SOAPY_SDR_TX, tx->dac, (double)gain);

  if (rc != 0) {
    t_print("soapy_protocol: SoapySDRDevice_setGain failed: %s\n", SoapySDR_errToStr(rc));
  }
}

void soapy_protocol_set_tx_gain_element(TRANSMITTER *tx, char *name, int gain) {
  int rc;
  rc = SoapySDRDevice_setGainElement(soapy_device, SOAPY_SDR_TX, tx->dac, name, (double)gain);

  if (rc != 0) {
    t_print("soapy_protocol: SoapySDRDevice_setGainElement %s failed: %s\n", name, SoapySDR_errToStr(rc));
  }
}

int soapy_protocol_get_gain_element(RECEIVER *rx, char *name) {
  double gain;
  gain = SoapySDRDevice_getGainElement(soapy_device, SOAPY_SDR_RX, rx->adc, name);
  return (int)gain;
}

int soapy_protocol_get_tx_gain_element(TRANSMITTER *tx, char *name) {
  double gain;
  gain = SoapySDRDevice_getGainElement(soapy_device, SOAPY_SDR_TX, tx->dac, name);
  return (int)gain;
}

// cppcheck-suppress unusedFunction
gboolean soapy_protocol_get_automatic_gain(RECEIVER *rx) {
  gboolean mode = SoapySDRDevice_getGainMode(soapy_device, SOAPY_SDR_RX, rx->adc);
  return mode;
}

void soapy_protocol_set_automatic_gain(RECEIVER *rx, gboolean mode) {
  int rc;
  rc = SoapySDRDevice_setGainMode(soapy_device, SOAPY_SDR_RX, rx->adc, mode);

  if (rc != 0) {
    t_print("soapy_protocol: SoapySDRDevice_getGainMode failed: %s\n", SoapySDR_errToStr(rc));
  }
}
