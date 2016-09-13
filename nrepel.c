/*
noise-repellent -- Noise Reduction LV2

Copyright 2016 Luciano Dato <lucianodato@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/
*/


#include <stdlib.h>
#include <string.h>
#include <fftw3.h>
#include <time.h>
#include <stdio.h>


#include "denoise_gain.c"

#include "lv2/lv2plug.in/ns/lv2core/lv2.h"
#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/atom/util.h"
#include "lv2/lv2plug.in/ns/ext/urid/urid.h"
#include "lv2/lv2plug.in/ns/ext/state/state.h"
#include "lv2/lv2plug.in/ns/ext/log/log.h"
#include "lv2/lv2plug.in/ns/ext/log/logger.h"

#define NREPEL_URI "https://github.com/lucianodato/noise-repellent"
#define NREPEL_NPRINT "http://example.org/noise-repellent/noise_print"

//STFT default values (This are standart values)
#define FFT_SIZE 2048 //max should be 8192 otherwise is too expensive
#define WINDOW_COMBINATION 0 //0 HANN-HANN 1 HAMMING-HANN 2 BLACKMAN-HANN
#define OVERLAP_FACTOR 4 //4 is 75% overlap
#define HANN_HANN_SCALING 0.375 //This is for overlapadd scaling
#define HAMMING_HANN_SCALING 0.385 // 1/average(window[i]^2)
#define BLACKMAN_HANN_SCALING 0.335

//Whitening strenght
#define WA 0.05 //For spectral whitening strenght 0-0.1


///---------------------------------------------------------------------

//LV2 CODE

typedef enum {
	NREPEL_CAPTURE = 0,
	NREPEL_N_AUTO = 1,
	NREPEL_AMOUNT = 2,
	NREPEL_STRENGHT = 3,
	NREPEL_SMOOTHING = 4,
	NREPEL_FREQUENCY_SMOOTHING = 5,
	NREPEL_LATENCY = 6,
	NREPEL_WHITENING = 7,
	NREPEL_RESET = 8,
	NREPEL_NOISE_LISTEN = 9,
	NREPEL_ENABLE = 10,
	NREPEL_INPUT = 11,
	NREPEL_OUTPUT = 12,
} PortIndex;

//URIs
typedef struct {
	LV2_URID atom_Vector;
	LV2_URID noise_print;
} NrepelURIs;

typedef struct {
	const float* input; //input of samples from host (changing size)
	float* output; //output of samples to host (changing size)
	float samp_rate; // Sample rate received from the host

	//Parameters for the algorithm (user input)
	float* capture_state; // Capture Noise state (Manual-Off-Auto)
	float* amount_of_reduction; // Amount of noise to reduce in dB
	float* over_sustraction; // Amount of noise to reduce in dB
	float* report_latency; // Latency necessary
	float* reset_print; // Latency necessary
	float* noise_listen; //For noise only listening
	float* residual_whitening; //Whitening of the residual spectrum
	float* time_smoothing; //constant that set the time smoothing coefficient
	float* auto_state; //autocapture switch
	float* frequency_smoothing; //Smoothing over frequency
	float* enable; //For soft bypass (click free bypass)


	//Parameters values and arrays for the STFT
	int fft_size; //FFTW input size
	int fft_size_2; //FFTW half input size
	int window_combination; // Window combination for the STFT
	float overlap_factor; //oversampling factor for overlap calculations
	float overlap_scale_factor; //Scaling factor for conserving the final amplitude
	int hop; // Hop size for the STFT
	float* window_input; // Input Window values
	float* window_output; // Input Window values
	float window_count; //Count windows for mean computing
	float max_float; //Auxiliary variable to store FLT_MAX and avoid warning
	float tau; //time constant for soft bypass
	float wet_dry_target; //softbypass target for softbypass
	float wet_dry; //softbypass
	float reduction_coeff; //Gain to apply to the residual noise

	//Buffers for processing and outputting
	int input_latency;
	float* in_fifo; //internal input buffer
	float* out_fifo; //internal output buffer
	float* output_accum; //FFT output accumulator
	int read_ptr; //buffers read pointer

	//FFTW related arrays
	float* input_fft_buffer;
	float* output_fft_buffer;
	fftwf_plan forward;
	fftwf_plan backward;

	//Arrays and variables for getting bins info
	float real_p,imag_n,mag,p2;
	float* fft_magnitude;//magnitude
	float* fft_p2;//power spectrum
	float* fft_p2_prev;//power spectum of previous frame
	float* noise_thresholds;

	float* Gk; //gain to be applied

	float* denoised_fft_buffer;
	float* residual_spectrum;

	float wa;
	float* tappering_filter;
	float* whitening_spectrum;

	float* auto_thresholds; //Reference threshold for louizou algorithm
	float* prev_noise_thresholds;
	float* s_pow_spec;
	float* prev_s_pow_spec;
	float* p_min;
	float* prev_p_min;
	float* speech_p_p;
	float* prev_speech_p_p;

	// clock_t start, end;
	// double cpu_time_used;

	// Features and URI mapping
  LV2_URID_Map* map;
	NrepelURIs uris;
	// Log feature and convenience API
	LV2_Log_Log* log;
  LV2_Log_Logger logger;

} Nrepel;

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
						double                    rate,
						const char*               bundle_path,
						const LV2_Feature* const* features) {

	//Actual struct declaration
	Nrepel* nrepel = (Nrepel*)malloc(sizeof(Nrepel));


  //FEATURES
	// Scan host features for URID map
	 LV2_URID_Map* map = NULL;
	 // Get host features
  for (int i = 0; features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_URID__map)) {
      nrepel->map = (LV2_URID_Map*)features[i]->data;
    } else if (!strcmp(features[i]->URI, LV2_LOG__log)) {
      nrepel->log = (LV2_Log_Log*)features[i]->data;
    }
  }
	if (!nrepel->map) {
    fprintf(stderr, "Noise Repellent error: Host does not support urid:map\n");
    free(nrepel);
    return NULL;
  }

	// Map URIs
	NrepelURIs* const uris = &nrepel->uris;
	nrepel->map = map;
	nrepel->uris.atom_Vector = map_uri(LV2_ATOM__Vector);
	nrepel->uris.noise_print = map_uri(NREPEL_NPRINT "Noise Print");
	//nrepel->state.noise_print = ; //State buffer initialization

	//Initialise logger
  lv2_log_logger_init(&nrepel->logger, nrepel->map, nrepel->log);

	//Initialize variables
	nrepel->samp_rate = rate;

	nrepel->in_fifo = (float*)calloc(nrepel->fft_size,sizeof(float));
	nrepel->out_fifo = (float*)calloc(nrepel->fft_size,sizeof(float));
	nrepel->output_accum = (float*)calloc(nrepel->fft_size,sizeof(float));

	nrepel->window_input = (float*)calloc(nrepel->fft_size,sizeof(float));
	nrepel->window_output = (float*)calloc(nrepel->fft_size,sizeof(float));

	nrepel->input_fft_buffer = (float*)calloc(nrepel->fft_size,sizeof(float));
	nrepel->output_fft_buffer = (float*)calloc(nrepel->fft_size,sizeof(float));
	nrepel->forward = fftwf_plan_r2r_1d(nrepel->fft_size, nrepel->input_fft_buffer, nrepel->output_fft_buffer, FFTW_R2HC, FFTW_ESTIMATE);
	nrepel->backward = fftwf_plan_r2r_1d(nrepel->fft_size, nrepel->output_fft_buffer, nrepel->input_fft_buffer, FFTW_HC2R, FFTW_ESTIMATE);

	nrepel->fft_magnitude = (float*)calloc((nrepel->fft_size_2+1),sizeof(float));
	nrepel->fft_p2 = (float*)calloc((nrepel->fft_size_2+1),sizeof(float));
	nrepel->fft_p2_prev = (float*)calloc((nrepel->fft_size_2+1),sizeof(float));
	nrepel->noise_thresholds = (float*)calloc((nrepel->fft_size_2+1),sizeof(float));
	nrepel->auto_thresholds = (float*)calloc((nrepel->fft_size_2+1),sizeof(float));

	nrepel->Gk = (float*)malloc((nrepel->fft_size_2+1)*sizeof(float));

	nrepel->denoised_fft_buffer = (float*)calloc(nrepel->fft_size,sizeof(float));
	nrepel->residual_spectrum = (float*)calloc((nrepel->fft_size),sizeof(float));

	nrepel->whitening_spectrum = (float*)calloc((nrepel->fft_size_2+1),sizeof(float));
	nrepel->tappering_filter = (float*)calloc(nrepel->fft_size_2+1,sizeof(float));

  nrepel->prev_noise_thresholds = (float*)calloc((nrepel->fft_size_2+1),sizeof(float));
	nrepel->s_pow_spec = (float*)calloc((nrepel->fft_size_2+1),sizeof(float));
	nrepel->prev_s_pow_spec = (float*)calloc((nrepel->fft_size_2+1),sizeof(float));
	nrepel->p_min = (float*)calloc((nrepel->fft_size_2+1),sizeof(float));
	nrepel->prev_p_min = (float*)calloc((nrepel->fft_size_2+1),sizeof(float));
	nrepel->speech_p_p = (float*)calloc((nrepel->fft_size_2+1),sizeof(float));
	nrepel->prev_speech_p_p = (float*)calloc((nrepel->fft_size_2+1),sizeof(float));

	return (LV2_Handle)nrepel;
}



static void
connect_port(LV2_Handle instance,
						 uint32_t   port,
						 void*      data) {
	Nrepel* nrepel = (Nrepel*)instance;

	switch ((PortIndex)port) {
		case NREPEL_CAPTURE:
		nrepel->capture_state = (float*)data;
		break;
		case NREPEL_N_AUTO:
		nrepel->auto_state = (float*)data;
		break;
		case NREPEL_AMOUNT:
		nrepel->amount_of_reduction = (float*)data;
		break;
		case NREPEL_STRENGHT:
		nrepel->over_sustraction = (float*)data;
		break;
		case NREPEL_SMOOTHING:
		nrepel->time_smoothing = (float*)data;
		break;
		case NREPEL_FREQUENCY_SMOOTHING:
		nrepel->frequency_smoothing = (float*)data;
		break;
		case NREPEL_LATENCY:
		nrepel->report_latency = (float*)data;
		break;
		case NREPEL_WHITENING:
		nrepel->residual_whitening = (float*)data;
		break;
		case NREPEL_RESET:
		nrepel->reset_print = (float*)data;
		break;
		case NREPEL_NOISE_LISTEN:
		nrepel->noise_listen = (float*)data;
		break;
		case NREPEL_ENABLE:
		nrepel->enable = (float*)data;
		break;
		case NREPEL_INPUT:
		nrepel->input = (const float*)data;
		break;
		case NREPEL_OUTPUT:
		nrepel->output = (float*)data;
		break;
	}
}

static void
activate(LV2_Handle instance)
{
	Nrepel* nrepel = (Nrepel*)instance;

	nrepel->fft_size = FFT_SIZE;
	nrepel->window_combination = WINDOW_COMBINATION;
	nrepel->overlap_factor = OVERLAP_FACTOR;
	nrepel->max_float = FLT_MAX;
	nrepel->wa = WA;
	nrepel->window_count = 0.f;
	nrepel->tau = (1.f - exp (-2.f * M_PI * 25.f * 64.f  / nrepel->samp_rate));

	nrepel->fft_size_2 = nrepel->fft_size/2;
	nrepel->hop = nrepel->fft_size/nrepel->overlap_factor;
	nrepel->input_latency = nrepel->fft_size - nrepel->hop;
	nrepel->read_ptr = nrepel->input_latency; //the initial position because we are that many samples ahead

	//Window combination computing
	switch(nrepel->window_combination){
		case 0: // HANN-HANN
			fft_window(nrepel->window_input,nrepel->fft_size,0); //STFT input window
			fft_window(nrepel->window_output,nrepel->fft_size,0); //STFT output window
			nrepel->overlap_scale_factor = HANN_HANN_SCALING;
			break;
		case 1: //HAMMING-HANN
			fft_window(nrepel->window_input,nrepel->fft_size,1); //STFT input window
			fft_window(nrepel->window_output,nrepel->fft_size,0); //STFT output window
			nrepel->overlap_scale_factor = HAMMING_HANN_SCALING;
			break;
		case 2: //BLACKMAN-HANN
			fft_window(nrepel->window_input,nrepel->fft_size,2); //STFT input window
			fft_window(nrepel->window_output,nrepel->fft_size,0); //STFT output window
			nrepel->overlap_scale_factor = BLACKMAN_HANN_SCALING;
			break;
	}

	//This is experimentally obteined
	int LF = Freq2Index(1000.f,nrepel->samp_rate,nrepel->fft_size);//1kHz
	int MF = Freq2Index(3000.f,nrepel->samp_rate,nrepel->fft_size);//3kHz
	for (int k = 0;k <= nrepel->fft_size_2; k++){
		if(k < LF){
			nrepel->auto_thresholds[k] = 2.f;
		}
		if(k > LF && k < MF){
			nrepel->auto_thresholds[k] = 2.f;
		}
		if(k > MF){
			nrepel->auto_thresholds[k] = 5.f;
		}
	}

	//Set initial gain to apply to unity
	memset(nrepel->Gk, 1, (nrepel->fft_size_2+1)*sizeof(float));

	//Tappering window
	tappering_filter_calc(nrepel->tappering_filter,(nrepel->fft_size_2+1),WA);

}

static void
run(LV2_Handle instance, uint32_t n_samples) {
	Nrepel* nrepel = (Nrepel*)instance;

	// //Time execution measurement
	// nrepel->start = clock();
	// //--------------

	//handy variables
	int k;
	unsigned int pos;

	//Inform latency at run call
	*(nrepel->report_latency) = (float) nrepel->input_latency;

	//Softbypass targets in case of disabled or enabled
	if(*(nrepel->enable) == 0.f){ //if disabled
		nrepel->wet_dry_target = 0.f;
	} else { //if enabled
		nrepel->wet_dry_target = 1.f;
	}

	//Interpolate parameters over time softly to bypass without clicks or pops
	nrepel->wet_dry += nrepel->tau * (nrepel->wet_dry_target - nrepel->wet_dry) + FLT_MIN;

	//Reset button state (if on)
	if (*(nrepel->reset_print) == 1.f) {
		memset(nrepel->noise_thresholds, 0, (nrepel->fft_size_2+1)*sizeof(float));
		memset(nrepel->residual_spectrum, 0, (nrepel->fft_size)*sizeof(float));
		memset(nrepel->Gk, 1, (nrepel->fft_size_2+1)*sizeof(float));
		nrepel->window_count = 0.f;

		memset(nrepel->prev_noise_thresholds, 0, (nrepel->fft_size_2+1)*sizeof(float));
		memset(nrepel->s_pow_spec, 0, (nrepel->fft_size_2+1)*sizeof(float));
		memset(nrepel->prev_s_pow_spec, 0, (nrepel->fft_size_2+1)*sizeof(float));
		memset(nrepel->p_min, 0, (nrepel->fft_size_2+1)*sizeof(float));
		memset(nrepel->prev_p_min, 0, (nrepel->fft_size_2+1)*sizeof(float));
		memset(nrepel->speech_p_p, 0, (nrepel->fft_size_2+1)*sizeof(float));
		memset(nrepel->prev_speech_p_p, 0, (nrepel->fft_size_2+1)*sizeof(float));
	}

	//main loop for processing
	for (pos = 0; pos < n_samples; pos++){
		//Store samples int the input buffer
		nrepel->in_fifo[nrepel->read_ptr] = nrepel->input[pos];
		//Output samples in the output buffer (even zeros introduced by latency)
		nrepel->output[pos] = nrepel->out_fifo[nrepel->read_ptr - nrepel->input_latency];
		//Now move the read pointer
		nrepel->read_ptr++;

		//Once the buffer is full we can do stuff
		if (nrepel->read_ptr >= nrepel->fft_size){
			//Reset the input buffer position
			nrepel->read_ptr = nrepel->input_latency;

			//Apply windowing
			for (k = 0; k < nrepel->fft_size; k++){
				nrepel->input_fft_buffer[k] = nrepel->in_fifo[k] * nrepel->window_input[k];
			}

			//----------FFT Analysis------------

			//Do transform
			fftwf_execute(nrepel->forward);

			//-----------GET INFO FROM BINS--------------

			//Normalize values to obtain correct magnitude and power values
			for (k = 0; k < nrepel->fft_size; k++){
				nrepel->output_fft_buffer[k] /= nrepel->fft_size;
			}

			//Get the positive spectrum and compute the magnitude
			for (k = 0; k <= nrepel->fft_size_2; k++){
				//Get the half complex spectrum reals and complex
				nrepel->real_p = nrepel->output_fft_buffer[k];
				nrepel->imag_n = nrepel->output_fft_buffer[nrepel->fft_size-k];

				//Get the magnitude and power spectrum
				if(k < nrepel->fft_size){
					nrepel->p2 = (nrepel->real_p*nrepel->real_p + nrepel->imag_n*nrepel->imag_n);
					nrepel->mag = sqrtf(nrepel->p2);//sqrt(real^2+imag^2)
				} else {
					//Nyquist - this is due to half complex transform look at http://www.fftw.org/doc/The-Halfcomplex_002dformat-DFT.html
					nrepel->p2 = nrepel->real_p*nrepel->real_p;
					nrepel->mag = nrepel->real_p;
				}

				//Store values in magnitude and power arrays
				nrepel->fft_p2_prev[k] = nrepel->fft_p2[k]; //store previous value for smoothing
				nrepel->fft_p2[k] = sanitize_denormal(nrepel->p2);
				nrepel->fft_magnitude[k] = sanitize_denormal(nrepel->mag);

			}

			//------------Processing---------------

			if(*(nrepel->auto_state) == 1.f) {
				float noise_reference_threshold = *(nrepel->auto_thresholds);

				//if slected auto estimate noise spectrum
				auto_capture_noise(nrepel->fft_p2,
													 nrepel->fft_size_2,
													 nrepel->noise_thresholds,
													 noise_reference_threshold,
													 nrepel->prev_noise_thresholds,
													 nrepel->s_pow_spec,
													 nrepel->prev_s_pow_spec,
													 nrepel->p_min,
													 nrepel->prev_p_min,
													 nrepel->speech_p_p,
													 nrepel->prev_speech_p_p);
			}

			if(*(nrepel->capture_state) == 1.f) { //MANUAL
				//If selected estimate noise spectrum based on selected portion of signal
				get_noise_statistics(nrepel->fft_p2,
														nrepel->fft_size_2,
														nrepel->noise_thresholds,
														&nrepel->window_count); //Use fixed value here
			} else {
				//DENOISE PRE PROCESSING

				//SMOOTHING
				//Time smoothing between current and past power spectrum
				for (k = 0; k <= nrepel->fft_size_2; k++) {
					nrepel->fft_p2[k] = (1.f - *(nrepel->time_smoothing)) * nrepel->fft_p2[k] + *(nrepel->time_smoothing) * nrepel->fft_p2_prev[k];
				}

				//Here we could smooth the frequency bins to further avoid musical noise (not a good choise for hum type od noise though)

				//Spectral Sustraction (Power Sustraction)
				denoise_gain_ss(*(nrepel->over_sustraction),
											nrepel->fft_size_2,
											nrepel->fft_p2,
											nrepel->noise_thresholds,
											nrepel->Gk);

				//Frequency smoothing of gains could be done here, but it proved to be a bad choise overall
				spectral_smoothing_SG_quad(nrepel->Gk,*(nrepel->frequency_smoothing),nrepel->fft_size_2);
			}
			//APPLY REDUCTION

			nrepel->reduction_coeff = (1.f/from_dB(*(nrepel->amount_of_reduction)));

			//Apply the computed gain to the signal and store it in denoised array
			for (k = 0; k <= nrepel->fft_size_2; k++) {
				nrepel->denoised_fft_buffer[k] = nrepel->output_fft_buffer[k]* nrepel->Gk[k];
				if(k < nrepel->fft_size_2)
					nrepel->denoised_fft_buffer[nrepel->fft_size-k] = nrepel->output_fft_buffer[nrepel->fft_size-k]* nrepel->Gk[k];
			}

			//Residual signal
			for (k = 0; k <= nrepel->fft_size_2; k++) {
			 nrepel->residual_spectrum[k] = nrepel->output_fft_buffer[k] - nrepel->denoised_fft_buffer[k];
			 if(k < nrepel->fft_size_2)
				nrepel->residual_spectrum[nrepel->fft_size-k] = nrepel->output_fft_buffer[nrepel->fft_size-k] - nrepel->denoised_fft_buffer[nrepel->fft_size-k];
			}

			//Residal signal Whitening and tappering
			if(*(nrepel->residual_whitening) == 1.f) {
				whitening_of_spectrum(nrepel->residual_spectrum,nrepel->wa,nrepel->fft_size_2);
				apply_tappering_filter(nrepel->residual_spectrum,nrepel->tappering_filter,nrepel->fft_size_2);
			}

			//Listen to cleaned signal or to noise only
			if (*(nrepel->noise_listen) == 0.f){
				//Mix residual and processed (Parametric way of noise reduction)
				for (k = 0; k <= nrepel->fft_size_2; k++) {
					nrepel->output_fft_buffer[k] =  (1.f-nrepel->wet_dry) * nrepel->output_fft_buffer[k] + (nrepel->denoised_fft_buffer[k] + nrepel->residual_spectrum[k]*nrepel->reduction_coeff) * nrepel->wet_dry;
					if(k < nrepel->fft_size_2)
						nrepel->output_fft_buffer[nrepel->fft_size-k] = (1.f-nrepel->wet_dry) * nrepel->output_fft_buffer[nrepel->fft_size-k] + (nrepel->denoised_fft_buffer[nrepel->fft_size-k] + nrepel->residual_spectrum[nrepel->fft_size-k]*nrepel->reduction_coeff) * nrepel->wet_dry;
				}
			} else {
				//Output noise only
				for (k = 0; k <= nrepel->fft_size_2; k++) {
					nrepel->output_fft_buffer[k] = (1.f-nrepel->wet_dry) * nrepel->output_fft_buffer[k] + nrepel->residual_spectrum[k] * nrepel->wet_dry;
					if(k < nrepel->fft_size_2)
						nrepel->output_fft_buffer[nrepel->fft_size-k] = (1.f-nrepel->wet_dry) * nrepel->output_fft_buffer[nrepel->fft_size-k] + nrepel->residual_spectrum[nrepel->fft_size-k] * nrepel->wet_dry;
				}
			}

			//------------FFT Synthesis-------------

			//Do inverse transform
			fftwf_execute(nrepel->backward);

			//------------OVERLAPADD-------------

			//Accumulate (Overlapadd)
			for(k = 0; k < nrepel->fft_size; k++){
				nrepel->output_accum[k] += nrepel->window_output[k]*nrepel->input_fft_buffer[k]/(float)(nrepel->overlap_scale_factor*nrepel->overlap_factor);
			}

			//Output samples up to the hop size
			for (k = 0; k < nrepel->hop; k++){
				nrepel->out_fifo[k] = nrepel->output_accum[k];
			}

			//shift FFT accumulator the hop size
			memmove(nrepel->output_accum, nrepel->output_accum + nrepel->hop, nrepel->fft_size*sizeof(float));

			//Make sure that the non overlaping section is 0
			for (k = (nrepel->fft_size-nrepel->hop); k < nrepel->fft_size; k++){
				nrepel->output_accum[k] = 0.f;
			}

			//move input FIFO
			for (k = 0; k < nrepel->input_latency; k++){
				nrepel->in_fifo[k] = nrepel->in_fifo[k+nrepel->hop];
			}
			//-------------------------------
		}//if
	}//main loop

	// //Time measurement
	// nrepel->end = clock();
	// nrepel->cpu_time_used = ((double) (nrepel->end - nrepel->start)) / CLOCKS_PER_SEC;
	//
	// //To string
	// char buffer[50];
	// sprintf(buffer,"%lf",nrepel->cpu_time_used);
	// strcat(buffer,"\n");
	//
	// //Saving results to a file
	// FILE *fp;
	//
	// fp = fopen("resuts.txt", "a");
	// fputs(buffer, fp);
	// fclose(fp);
}

static void
deactivate(LV2_Handle instance)
{
}

static void
cleanup(LV2_Handle instance)
{
	free(instance);
}

static LV2_State_Status
state_save(LV2_Handle                instance,
           LV2_State_Store_Function  store,
           LV2_State_Handle          handle,
           uint32_t                  flags,
           const LV2_Feature* const* features) {
    Nrepel* nrepel = (Nrepel*)instance;

		if (!nrepel) {
      return LV2_STATE_SUCCESS;
    }

    store(handle, nrepel->uris.noise_print,
          (void*)&nrepel->noise_thresholds, sizeof(float)*nrepel->fft_size,
          nrepel->uris.atom_Vector,
          LV2_STATE_IS_POD);

    return LV2_STATE_SUCCESS;
}

static LV2_State_Status
state_restore(LV2_Handle                  instance,
              LV2_State_Retrieve_Function retrieve,
              LV2_State_Handle            handle,
              uint32_t                    flags,
              const LV2_Feature* const*   features) {
    Nrepel* nrepel = (Nrepel*)instance;

    size_t   size;
    uint32_t type;
    uint32_t valflags;

    float* noise_print = retrieve(handle, nrepel->uris.noise_print, &size, &type, &valflags);
    if (noise_print && size == sizeof(float)*nrepel->fft_size && type == nrepel->uris.atom_Vector) {
      nrepel->noise_thresholds = ((float*)noise_print);
    }

    return LV2_STATE_SUCCESS;
}

static const void*
extension_data(const char* uri) {
  static const LV2_State_Interface state = { state_save, state_restore };
  if (!strcmp(uri, LV2_STATE__interface)) {
    return &state;
  }
  return NULL;
}

static const
LV2_Descriptor descriptor = {
	NREPEL_URI,
	instantiate,
	connect_port,
	activate,
	run,
	deactivate,
	cleanup,
	extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
	switch (index) {
		case 0:
		return &descriptor;
		default:
		return NULL;
	}
}
