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


#include <math.h>
#include <float.h>
#ifndef __cplusplus
#    include <stdbool.h>
#endif

//Window types
#define HANN_WINDOW 0
#define HAMMING_WINDOW 1
#define BLACKMAN_WINDOW 2

#ifndef M_PI
#    define M_PI 3.14159265
#endif

//AUXILIARY Functions

#define MAX(a,b) (((a) > (b)) ? (a) : (b))
#define MIN(a,b) (((a) < (b)) ? (a) : (b))

// Force already-denormal float value to zero
inline float sanitize_denormal(float value) {
  if (isnan(value)) {
    return FLT_MIN; //to avoid log errors
    //return 0.f; //to avoid log errors
  } else {
    return value;
  }

}

inline float Index2Freq(int i, float samp_rate, int N) {
  return (float) i * (samp_rate / N / 2.f);
}

inline int Freq2Index(float freq, float samp_rate, int N) {
  return (int) (freq / (samp_rate / N / 2.f));
}

inline int sign(float x) {
  return (x >= 0.f ? 1.f : -1.f);
}

inline float from_dB(float gdb) {
  return (expf(gdb/20.f*logf(10.f)));
}

inline float to_dB(float g) {
  return (20.f*log10f(g));
}

inline float mean(int m, float* a) {
    int sum=0, i;
    for(i=0; i<m; i++)
        sum+=a[i];
    return((float)sum/m);
}

inline float median(int n, float* x) {
    float temp;
    int i, j;
    // the following two loops sort the array x in ascending order
    for(i=0; i<n-1; i++) {
        for(j=i+1; j<n; j++) {
            if(x[j] < x[i]) {
                // swap elements
                temp = x[i];
                x[i] = x[j];
                x[j] = temp;
            }
        }
    }

    if(n%2==0) {
        // if there is an even number of elements, return mean of the two elements in the middle
        return((x[n/2] + x[n/2 - 1]) / 2.f);
    } else {
        // else return the element in the middle
        return x[n/2];
    }
}

inline float moda(int n, float* x) {
  float temp[n];
  int i,j,pos_max;
  float max;

  for(i = 0;i<n; i++) {
      temp[i]=0.f;
  }

  for(i=0; i<n; i++) {
      for(j=i; j<n; j++) {
          if(x[j] == x[i]) temp[i]++;
      }
  }

  max=temp[0];
  pos_max = 0;
  for(i=0; i<n; i++) {
      if(temp[i] > max) {
          pos_max = i;
          max=temp[i];
      }
  }
  return x[pos_max];
}

inline float blackman(int k, int N) {
  float p = ((float)(k))/((float)(N));
  return 0.42-0.5*cosf(2.f*M_PI*p) + 0.08*cosf(4.f*M_PI*p);
}

inline float hanning(int k, int N) {
  float p = ((float)(k))/((float)(N));
  return 0.5 - 0.5 * cosf(2.f*M_PI*p);
}

inline float hamming(int k, int N) {
  float p = ((float)(k))/((float)(N));
  return 0.54 - 0.46 * cosf(2.f*M_PI*p);
}

void fft_window(float* window, int N, int window_type) {
  int k;
  for (k = 0; k < N; k++){
    switch (window_type){
      case BLACKMAN_WINDOW:
      window[k] = blackman(k, N);
      break;
      case HANN_WINDOW:
      window[k] = hanning(k, N);
      break;
      case HAMMING_WINDOW:
      window[k] = hamming(k, N);
      break;
    }
  }
}

inline bool is_empty(float* noise_print, int N){
  int k;
  float sum = 0.f;
  for(k = 0;k <= N; k++){
    sum += noise_print[k];
  }
  if(sum > 0){
    return true;
  }
  return false;
}
inline float max_spectral_value(float* noise_print, int N){
  int k;
  float max = 0.f;
  for(k = 0; k <= N; k++){
    max = MAX(noise_print[k],max);
  }
  return max;
}

inline float min_spectral_value(float* noise_print, int N){
  int k;
  float min = FLT_MAX;
  for(k = 0; k <= N; k++){
    min = MIN(noise_print[k],min);
  }
  return min;
}

//unnormalized Hann windows for whitening tappering
void tappering_filter_calc(float* filter, int N,float wa) {
  int k;
  for (k = 0; k < N; k++){
    filter[k] = hanning(k, N);//Half hann window tappering in favor of high frequencies
  }
}

void whitening_of_spectrum(float* spectrum,float wa,int N){
  float whitened_spectrum[N+1];
  float tmp_min = min_spectral_value(spectrum,N);
  float tmp_max = max_spectral_value(spectrum,N);
  for (int k = 0; k <= N; k++) {
    if(spectrum[k] > FLT_MIN){ //Protects against division by 0
      whitened_spectrum[k] = powf((spectrum[k]/(tmp_max-tmp_min)),wa);
      spectrum[k] /= whitened_spectrum[k];
      if(k < N){
        spectrum[N-k] /= whitened_spectrum[k];
      }
    }
  }
}

void apply_tappering_filter(float* spectrum,float* filter,int N) {
  for (int k = 0; k <= N; k++) {
    if(spectrum[k] > FLT_MIN) {
      spectrum[k] *= filter[k];//Half hann window tappering in favor of high frequencies
      if(k < N) {
        spectrum[N-k] *= filter[k];//Half hann window tappering in favor of high frequencies
      }
    }
  }
}


//---------------------SMOOTHERS--------------------------


//Spectral smoothing with rectangular boxcar or unweighted sliding-average smooth
void spectral_smoothing_MA(float* spectrum, int kernel_width,int N){
  int k;
  float smoothing_tmp[N+1];
  float t_spectrum[N+1];

  if (kernel_width == 0) return;

  //Initialize smothingbins_tmp
  for (k = 0; k <= N; ++k) {
    t_spectrum[k] = spectrum[k];
    smoothing_tmp[k] = 0.f;//Initialize temporal spectrum
  }

  for (k = 0; k <= N; ++k) {
    const int j0 = MAX(0, k - kernel_width);
    const int j1 = MIN(N, k + kernel_width);
    for(int l = j0; l <= j1; ++l) {
      smoothing_tmp[k] += t_spectrum[l];
    }
    smoothing_tmp[k] /= (j1 - j0 + 1);
  }

  for (k = 0; k <= N; ++k){
    spectrum[k] = smoothing_tmp[k];
  }
}
//Spectral smoothing with median filter
void spectral_smoothing_MM(float* spectrum, int kernel_width, int N){
  int k;
  float smoothing_tmp[N+1];

  if (kernel_width == 0) return;

  for (k = 0; k <= N; ++k) {
    const int j0 = MAX(0, k - kernel_width);
    const int j1 = MIN(N, k + kernel_width);

    float aux[j1-j0+1];
    for(int l = j0; l <= j1; ++l) {
      aux[l] = spectrum[l];
    }
    smoothing_tmp[k] = median(j1-j0+1,aux);
  }

  for (k = 0; k <= N; ++k){
    spectrum[k] = smoothing_tmp[k];
  }
}


void spectral_smoothing_MAH(float* spectrum, int kernel_width,int N){
  int k;
  float smoothing_tmp[N+1];
  float extended[N+2*kernel_width+1];
  float window[kernel_width*2 +1];
  float win_sum = 0.f;
  fft_window(window,kernel_width*2+1,0);//Hann window

  if (kernel_width == 0) return;

  //Copy data over the extended array to contemplate edge cases
  //Initialize smothingbins_tmp
  for (k = 0; k <= N; ++k) {
    extended[k+kernel_width] = spectrum[k];
    smoothing_tmp[k] = 0.f; //Initialize with zeros
  }

  for (k = 0; k <= kernel_width*2; ++k) {
    win_sum += window[k];
  }


  for (k = 0; k <= N; ++k) {
    for(int l = 0; l <= kernel_width*2; ++l) {
      smoothing_tmp[k] += window[l]*extended[k+l]/win_sum;
    }
  }

  for (k = 0; k <= N; ++k){
    spectrum[k] = smoothing_tmp[k];
  }
}

//This is from wikipedia ;)
float savgol_quad_5[5] = {-0.085714,0.342857,0.485714,0.342857,-0.085714};
float savgol_quad_7[7] = {-0.095238,0.142857,0.285714,0.333333,0.285714,0.142857,-0.095238};
float savgol_quad_9[9] = {-0.090909,0.060606,0.168831,0.233766,0.255411,0.233766,0.168831,0.060606,-0.090909};
float savgol_quad_11[11] = {-0.083916,0.020979,0.102564,0.160839,0.195804,0.207459,0.195804,0.160839,0.102564,0.020979,-0.083916};
float savgol_quad_13[13] = {-0.076923,0.000000,0.062937,0.111888,0.146853,0.167832,0.174825,0.167832,0.146853,0.111888,0.062937,0.000000,-0.076923};
float savgol_quad_15[15] = {-0.070588,-0.011765,0.038009,0.078733,0.110407,0.133032,0.146606,0.151131,0.146606,0.133032,0.110407,0.078733,0.038009,-0.011765,-0.070588};
float savgol_quad_17[17] = {-0.065015,-0.018576,0.021672,0.055728,0.083591,0.105263,0.120743,0.130031,0.133127,0.130031,0.120743,0.105263,0.083591,0.055728,0.021672,-0.018576,-0.065015};
float savgol_quad_19[19] = {-0.060150,-0.022556,0.010615,0.039363,0.063689,0.083591,0.099071,0.110128,0.116762,0.118974,0.116762,0.110128,0.099071,0.083591,0.063689,0.039363,0.010615,-0.022556,-0.060150};
float savgol_quad_21[21] = {-0.055901,-0.024845,0.002942,0.027460,0.048709,0.066688,0.081399,0.092841,0.101013,0.105917,0.107551,0.105917,0.101013,0.092841,0.081399,0.066688,0.048709,0.027460,0.002942,-0.024845,-0.055901};
float savgol_quad_23[23] = {-0.052174,-0.026087,-0.002484,0.018634,0.037267,0.053416,0.067081,0.078261,0.086957,0.093168,0.096894,0.098137,0.096894,0.093168,0.086957,0.078261,0.067081,0.053416,0.037267,0.018634,-0.002484,-0.026087,-0.052174};
float savgol_quad_25[25] = {-0.048889,-0.026667,-0.006377,0.011981,0.028406,0.042899,0.055459,0.066280,0.074783,0.081546,0.086377,0.089275,0.090242,0.089275,0.086377,0.081546,0.074783,0.066280,0.055459,0.042899,0.028406,0.011981,-0.006377,-0.026667,-0.048889};

//With quadratic coefficients
void spectral_smoothing_SG_quad(float* spectrum, int kernel_width,int N){
  int k;
  float smoothing_tmp[N+1];
  float extended[N+1+2*kernel_width];

  if (kernel_width < 2 || kernel_width > 12) return;

  //Copy data over the extended array to contemplate edge cases
  for (k = 0; k <= N; ++k) {
    extended[k+kernel_width] = spectrum[k];
    smoothing_tmp[k] = 0.f; //Initialize with zeros
  }

  for (k = 0; k <= N; ++k) {
    for(int l = 0; l <= kernel_width*2; ++l) {
      switch(kernel_width){
        case 2:
          smoothing_tmp[k] += savgol_quad_5[l]*extended[l+k];
          break;
        case 3:
          smoothing_tmp[k] += savgol_quad_7[l]*extended[l+k];
          break;
        case 4:
          smoothing_tmp[k] += savgol_quad_9[l]*extended[l+k];
          break;
        case 5:
          smoothing_tmp[k] += savgol_quad_11[l]*extended[l+k];
          break;
        case 6:
          smoothing_tmp[k] += savgol_quad_13[l]*extended[l+k];
          break;
        case 7:
          smoothing_tmp[k] += savgol_quad_15[l]*extended[l+k];
          break;
        case 8:
          smoothing_tmp[k] += savgol_quad_17[l]*extended[l+k];
          break;
        case 9:
          smoothing_tmp[k] += savgol_quad_19[l]*extended[l+k];
          break;
        case 10:
          smoothing_tmp[k] += savgol_quad_21[l]*extended[l+k];
          break;
        case 11:
          smoothing_tmp[k] += savgol_quad_23[l]*extended[l+k];
          break;
        case 12:
          smoothing_tmp[k] += savgol_quad_25[l]*extended[l+k];
          break;
      }
    }
  }

  for (k = 0; k <= N; ++k){
    spectrum[k] = smoothing_tmp[k];
  }
}

//This is from wikipedia ;)
float savgol_quart_7[7] = {0.021645,-0.129870,0.324675,0.567100,0.324675,-0.129870,0.021645};
float savgol_quart_9[9] = {0.034965,-0.128205,0.069930,0.314685,0.417249,0.314685,0.069930,-0.128205,0.034965};

//With quadric coefficients
void spectral_smoothing_SG_quart(float* spectrum, int kernel_width,int N){
  int k;
  float smoothing_tmp[N+1];
  float extended[N+2*kernel_width+1];

  if (kernel_width < 3 || kernel_width > 4) return;

  //Initialize smothingbins_tmp
  for (k = 0; k <= N; ++k) {
    extended[k+kernel_width] = spectrum[k];
    smoothing_tmp[k] = 0.f; //Initialize with zeros
  }

  for (k = 0; k <= N; ++k) {
    for(int l = 0; l < kernel_width*2; ++l) {
      switch(kernel_width){
        case 3:
          smoothing_tmp[k] += savgol_quart_7[l]*extended[l+k];
          break;
        case 4:
          smoothing_tmp[k] += savgol_quart_9[l]*extended[l+k];
          break;
      }
    }
  }

  for (k = 0; k <= N; ++k){
    spectrum[k] = smoothing_tmp[k];
  }
}
