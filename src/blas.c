#include "blas.h"
#include "omp.h"
#include <stdint.h>

#include <math.h>
#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int max_abs(int src, int max_val)
{
    if (abs(src) > abs(max_val)) src = (src > 0) ? max_val : -max_val;
    return src;
}

/*************************************************************************************************************************
                        This funtion is main to realize the fake quantization in the paper of

                                "Quantization and Training of Neural Networks for Efficient 
                                        Integer-Arithmetic-Only Inference"
                        
                         We propose an approach that simulates quantization effects in the 
                         forward pass of training. Backpropagation still happens as usual, 
                             and all weights and biases are stored in floating point
 *************************************************************************************************************************/
void fake_quant_with_min_max_channel(int size_channel, float *input, uint8_t *input_int8, int size_feature, float *min_activ_value, float *max_activ_value, 
                                float *quantzation_scale, uint8_t *quantization_zero_point, int func_type, float decay) 
{
    for(int i = 0; i < size_channel; ++i){
        //Calculate min and max value of each kernel
        //because out_mul is calculate by input_mul and weights_mul, so I can only set size_channel to 1 for input because of gemm shape error
        float min_value = 0.0;
        float max_value = 0.0;
        int quant_min = QUANT_NEGATIVE_LIMIT; 
        int quant_max = QUANT_POSITIVE_LIMIT;
        for(int j = 0; j < size_feature; ++j){
            int index = i*size_feature+j;
            max_value = max(input[index], max_value);
            min_value = min(input[index], min_value);
        }
        if(func_type == INPUT_QUANT) printf("Input max = %.3f, min = %.3f\n",max_value, min_value);
        if(func_type == ACTIV_QUANT) printf("Activ max = %.3f, min = %.3f\n", max_value, min_value);
        //If this layer is activation, you need to update the min and max value with EMA 
        if(func_type == ACTIV_QUANT || func_type == INPUT_QUANT){
            min_activ_value[i] = min_activ_value[i] - ((min_activ_value[i] - min_value) * (1- decay));
            max_activ_value[i] = max_activ_value[i] - ((max_activ_value[i] - max_value) * (1- decay));
            max_value = max_activ_value[i];
            min_value = min_activ_value[i];
        }        
        // If min and max are both zero, we should just return zero.
        assert(min_value != 0.0f);
        assert(max_value != 0.0f);


        float nudged_scale = 0.0f;
        //this is really nudge function
        const float quant_min_float = (float)quant_min;
        const float quant_max_float = (float)quant_max;
        assert(quant_min_float != quant_max_float);
        nudged_scale = (max_value - min_value) / (quant_max_float - quant_min_float);
        assert(nudged_scale != 0);
        const double initial_zero_point = quant_min_float - min_value / nudged_scale;

        //Store the S3 for activ quantization, convenient for us to quantization input in inference process
        quantzation_scale[i] = nudged_scale;
        uint8_t nudged_zero_point = 0;
        if (initial_zero_point < quant_min) {
            nudged_zero_point = quant_min;
        } else if (initial_zero_point > quant_max) {
            nudged_zero_point = quant_max;
        } else {
            nudged_zero_point = round(initial_zero_point);
        }
        quantization_zero_point[i] = nudged_zero_point;

        if(func_type != INPUT_QUANT){
            const float nudged_scale_repl = nudged_scale;
            for(int k = 0; k < size_feature; ++k){
                int index_kernel = i*size_feature+k;
                float clamped = max(min_value, min(max_value, input[index_kernel]));
                float clamped_shifted = clamped - min_value;
                if(func_type == WEIGHT_QUANT){
                    input_int8[index_kernel] = round(clamped_shifted / nudged_scale_repl + 0.01f);
                }
                float temp = round(clamped_shifted / nudged_scale_repl + 0.01f) * nudged_scale_repl + min_value;
                input[index_kernel] = temp;
            }
        }
    }
}

// actually we don't need to quantization weights, because I got them from weights filt
void quantization_weights_and_activations(network *net)
{
    int i;    
    for (i = 0; i < net->n; ++i) {
        layer *l = &net->layers[i];
        if (l->type == CONVOLUTIONAL && l->layer_quant_flag){
            if(l->batch_normalize){
                assert(l->groups != 0);
                batch_normalize_weights(l->weights, l->rolling_variance, l->scales, l->out_c, l->size*l->size*l->c/l->groups); 
                batch_normalize_bias(l->biases, l->rolling_mean, l->rolling_variance, l->scales, l->out_c); 
            }
            for(int j = 0; j < l->nweights; ++j){
                assert(l->weight_data_uint8_scales[0] != 0);
                l->weights_uint8[j] = round(l->weights[j] / l->weight_data_uint8_scales[0]) + l->weight_data_int8_zero_point[0];
                l->weights_uint8[j] = clamp(l->weights_uint8[j], QUANT_NEGATIVE_LIMIT, QUANT_POSITIVE_LIMIT);
            }
            // *l->input_data_uint8_scales = *last_l->activ_data_uint8_scales;
            // *l->input_data_int8_zero_point = *last_l->activ_data_int8_zero_point;
            if (i > 0)
            {
                l->input_data_uint8_scales[0] = net->layers[i-1].activ_data_uint8_scales[0];
                l->input_data_int8_zero_point = net->layers[i-1].activ_data_int8_zero_point;
            }
            
            l->mult_zero_point = l->c*l->size*l->size*l->input_data_int8_zero_point[0]*l->weight_data_int8_zero_point[0];

            for(int ii = 0; ii < l->n; ++ii){
                for (int jj = 0; jj < l->c*l->size*l->size; ++jj){
                    l->weights_sum_int[ii] += l->weights_uint8[ii*l->c*l->size*l->size+jj];
                }
                    l->weights_sum_int[ii] =  l->mult_zero_point - l->weights_sum_int[ii] * l->input_data_int8_zero_point[0];
            }
            assert(l->activ_data_uint8_scales[0] != 0);
            l->M = l->input_data_uint8_scales[0] * l->weight_data_uint8_scales[0] / l->activ_data_uint8_scales[0];
            printf("layer:  %d, type:  [%s], input quant scale:   %f, input quant zero_p:   %d\n", l->count, "CONV", l->input_data_uint8_scales[0], l->input_data_int8_zero_point[0]);
            printf("layer:  %d, type:  [%s], weights quant scale: %f, weights quant zero_p: %d\n", l->count, "CONV", l->weight_data_uint8_scales[0], l->weight_data_int8_zero_point[0]);
            printf("layer:  %d, type:  [%s], activ quant scale:   %f, activ quant zero_p:   %d\n", l->count, "CONV", l->activ_data_uint8_scales[0], l->activ_data_int8_zero_point[0]);
            printf("----------------------------\n");

            quant_multi_smaller_than_one_to_scale_and_shift(l->M, &l->M0, &l->M0_right_shift);
            for(int jj = 0; jj < l->out_c; ++jj){
                assert(l->input_data_uint8_scales[0] != 0);
                l->biases_int32[jj] = l->biases[jj] / (l->input_data_uint8_scales[0] * l->weight_data_uint8_scales[0]);
            }

        }
        int inheritance_type = (l->type == MAXPOOL || l->type == ROUTE);
        if(inheritance_type && l->layer_quant_flag){
            printf("layer:  %d, type:  [%s], activ quant scale:   %f, activ quant zero_p:   %d\n", l->count, "POOL", l->activ_data_uint8_scales[0], l->activ_data_int8_zero_point[0]);
            printf("----------------------------\n");
            // l->activ_data_uint8_scales[0] =  net->layers[i-1].activ_data_uint8_scales[0];
            // l->activ_data_int8_zero_point[0] =  net->layers[i-1].activ_data_int8_zero_point[0];
        }
    }
}

/*************************************************************************************************************************
     Given a real_multiplier in the interval (0, 1),
    produces a pair (quantized_multiplier, right_shift) where
    quantized_multiplier is an int32 representing a fixed-point value
    in the interval [-1, 1)  (in practice we only produce positive values)
    and right_shift is an amount to shift right by, so that the
    floating-point multiplication of some int32 input value by real_multiplier,

    return static_cast<int32>(int32_value * real_multiplier);

    is best approximated by the integer-arithmetic-only code

    This is how to obtain the fixed-point multiplier and right shift
    parameters to pass to OutputStageQuantizeDownInt32ByFixedPoint.

 *************************************************************************************************************************/
void quant_multi_smaller_than_one_to_scale_and_shift(float real_multiplier,
                                      int32_t* quantized_multiplier,
                                      int* right_shift) 
{
    assert(real_multiplier > 0.f);
    assert(real_multiplier < 1.f);
    int s = 0;
    // We want to bring the real multiplier into the interval [1/2, 1).
    // We can do so by multiplying it by two, and recording how many times
    // we multiplied by two so that we can compensate that by a right
    // shift by the same amount.
    while (real_multiplier < 0.5f) {
        real_multiplier *= 2.0f;
        s++;
    }
    // Now that the real multiplier is in [1/2, 1), we convert it
    // into a fixed-point number.
    int64_t q = (round(real_multiplier * (1ll << 31)));
    assert(q <= (1ll << 31));
    // Handle the special case when the real multiplier was so close to 1
    // that its fixed-point approximation was undistinguishable from 1.
    // We handle this by dividing it by two, and remembering to decrement
    // the right shift amount.
    if (q == (1ll << 31)) {
        q /= 2;
        s--;
    }
    assert(s >= 0);
    assert(q <= pow(2, 32));
    *quantized_multiplier = (int32_t)(q);
    *right_shift = s;
}

void reorg_cpu(float *x, int w, int h, int c, int batch, int stride, int forward, float *out)
{
    int b,i,j,k;
    int out_c = c/(stride*stride);

    for(b = 0; b < batch; ++b){
        for(k = 0; k < c; ++k){
            for(j = 0; j < h; ++j){
                for(i = 0; i < w; ++i){
                    int in_index  = i + w*(j + h*(k + c*b));
                    int c2 = k % out_c;
                    int offset = k / out_c;
                    int w2 = i*stride + offset % stride;
                    int h2 = j*stride + offset / stride;
                    int out_index = w2 + w*stride*(h2 + h*stride*(c2 + out_c*b));
                    if(forward) out[out_index] = x[in_index];
                    else out[in_index] = x[out_index];
                }
            }
        }
    }
}

int clamp(int input, int min, int max)
{
    if (input < min){
        input = min;
    }
    if (input > max){
        input = max;
    }
    return input;
}

void flatten(float *x, int size, int layers, int batch, int forward)
{
    float *swap = calloc(size*layers*batch, sizeof(float));
    int i,c,b;
    for(b = 0; b < batch; ++b){
        for(c = 0; c < layers; ++c){
            for(i = 0; i < size; ++i){
                int i1 = b*layers*size + c*size + i;
                int i2 = b*layers*size + i*layers + c;
                if (forward) swap[i2] = x[i1];
                else swap[i1] = x[i2];
            }
        }
    }
    memcpy(x, swap, size*layers*batch*sizeof(float));
    free(swap);
}

void weighted_sum_cpu(float *a, float *b, float *s, int n, float *c)
{
    int i;
    for(i = 0; i < n; ++i){
        c[i] = s[i]*a[i] + (1-s[i])*(b ? b[i] : 0);
    }
}

void weighted_delta_cpu(float *a, float *b, float *s, float *da, float *db, float *ds, int n, float *dc)
{
    int i;
    for(i = 0; i < n; ++i){
        if(da) da[i] += dc[i] * s[i];
        if(db) db[i] += dc[i] * (1-s[i]);
        ds[i] += dc[i] * (a[i] - b[i]);
    }
}

void shortcut_cpu(int batch, int w1, int h1, int c1, float *add, int w2, int h2, int c2, float s1, float s2, float *out)
{
    int stride = w1/w2;
    int sample = w2/w1;
    assert(stride == h1/h2);
    assert(sample == h2/h1);
    if(stride < 1) stride = 1;
    if(sample < 1) sample = 1;
    int minw = (w1 < w2) ? w1 : w2;
    int minh = (h1 < h2) ? h1 : h2;
    int minc = (c1 < c2) ? c1 : c2;

    int i,j,k,b;
    for(b = 0; b < batch; ++b){
        for(k = 0; k < minc; ++k){
            for(j = 0; j < minh; ++j){
                for(i = 0; i < minw; ++i){
                    int out_index = i*sample + w2*(j*sample + h2*(k + c2*b));
                    int add_index = i*stride + w1*(j*stride + h1*(k + c1*b));
                    out[out_index] = s1*out[out_index] + s2*add[add_index];
                }
            }
        }
    }
}

void mean_cpu(float *x, int batch, int filters, int spatial, float *mean)
{
    float scale = 1./(batch * spatial);
    int i,j,k;
    for(i = 0; i < filters; ++i){
        mean[i] = 0;
        for(j = 0; j < batch; ++j){
            for(k = 0; k < spatial; ++k){
                int index = j*filters*spatial + i*spatial + k;
                mean[i] += x[index];
            }
        }
        mean[i] *= scale;
    }
}

void variance_cpu(float *x, float *mean, int batch, int filters, int spatial, float *variance)
{
    float scale = 1./(batch * spatial - 1);
    int i,j,k;
    for(i = 0; i < filters; ++i){
        variance[i] = 0;
        for(j = 0; j < batch; ++j){
            for(k = 0; k < spatial; ++k){
                int index = j*filters*spatial + i*spatial + k;
                variance[i] += pow((x[index] - mean[i]), 2);
            }
        }
        variance[i] *= scale;
    }
}

void l2normalize_cpu(float *x, float *dx, int batch, int filters, int spatial)
{
    int b,f,i;
    for(b = 0; b < batch; ++b){
        for(i = 0; i < spatial; ++i){
            float sum = 0;
            for(f = 0; f < filters; ++f){
                int index = b*filters*spatial + f*spatial + i;
                sum += powf(x[index], 2);
            }
            sum = sqrtf(sum);
            for(f = 0; f < filters; ++f){
                int index = b*filters*spatial + f*spatial + i;
                x[index] /= sum;
                dx[index] = (1 - x[index]) / sum;
            }
        }
    }
}


void normalize_cpu(float *x, float *mean, float *variance, int batch, int filters, int spatial)
{
    int b, f, i;
    for(b = 0; b < batch; ++b){
        for(f = 0; f < filters; ++f){
            for(i = 0; i < spatial; ++i){
                int index = b*filters*spatial + f*spatial + i;
                x[index] = (x[index] - mean[f])/(sqrt(variance[f]) + .000001f);
            }
        }
    }
}

void batch_normalize_weights(float *weights, float *variance, float *scales, int filters, int spatial)
{
	int i = 0,j = 0;
	//#pragma omp parallel for num_threads(8)
	for(i = 0; i < filters; i++){
		for(j = 0; j < spatial; j++){
			int weights_index = i*spatial + j;
			weights[weights_index] = weights[weights_index]*scales[i]/(sqrt(variance[i]) + .000001f);
		}
	}
}
	
void batch_normalize_bias(float *biases, float *rolling_mean, float *rolling_variance, float *scales, int filters)
{
	int i = 0;
	//#pragma omp parallel for num_threads(8)
	for(i = 0; i < filters; i++){
        biases[i] = biases[i]-scales[i]*rolling_mean[i]/(sqrt(rolling_variance[i]) + .000001f);
	}
}
void const_cpu(int N, float ALPHA, float *X, int INCX)
{
    int i;
    for(i = 0; i < N; ++i) X[i*INCX] = ALPHA;
}

void mul_cpu(int N, float *X, int INCX, float *Y, int INCY)
{
    int i;
    for(i = 0; i < N; ++i) Y[i*INCY] *= X[i*INCX];
}

void pow_cpu(int N, float ALPHA, float *X, int INCX, float *Y, int INCY)
{
    int i;
    for(i = 0; i < N; ++i) Y[i*INCY] = pow(X[i*INCX], ALPHA);
}

void axpy_cpu(int N, float ALPHA, float *X, int INCX, float *Y, int INCY)
{
    int i;
    for(i = 0; i < N; ++i) Y[i*INCY] += ALPHA*X[i*INCX];
}

void scal_cpu(int N, float ALPHA, float *X, int INCX)
{
    int i;
    for(i = 0; i < N; ++i) X[i*INCX] *= ALPHA;
}

void fill_cpu(int N, float ALPHA, float *X, int INCX)
{
    int i;
    for(i = 0; i < N; ++i) X[i*INCX] = ALPHA;
}

void deinter_cpu(int NX, float *X, int NY, float *Y, int B, float *OUT)
{
    int i, j;
    int index = 0;
    for(j = 0; j < B; ++j) {
        for(i = 0; i < NX; ++i){
            if(X) X[j*NX + i] += OUT[index];
            ++index;
        }
        for(i = 0; i < NY; ++i){
            if(Y) Y[j*NY + i] += OUT[index];
            ++index;
        }
    }
}

void inter_cpu(int NX, float *X, int NY, float *Y, int B, float *OUT)
{
    int i, j;
    int index = 0;
    for(j = 0; j < B; ++j) {
        for(i = 0; i < NX; ++i){
            OUT[index++] = X[j*NX + i];
        }
        for(i = 0; i < NY; ++i){
            OUT[index++] = Y[j*NY + i];
        }
    }
}

void copy_cpu(int N, float *X, int INCX, float *Y, int INCY)
{
    int i;
    for(i = 0; i < N; ++i) Y[i*INCY] = X[i*INCX];
}

void copy_cpu_int8(int N, int8_t *X, int INCX, int8_t *Y, int INCY)
{
    int i;
    for(i = 0; i < N; ++i) Y[i*INCY] = X[i*INCX];
}

void copy_cpu_uint8(int N, uint8_t *X, int INCX, uint16_t *Y, int INCY)
{
    int i;
    for(i = 0; i < N; ++i) Y[i*INCY] = X[i*INCX];
}

void mult_add_into_cpu(int N, float *X, float *Y, float *Z)
{
    int i;
    for(i = 0; i < N; ++i) Z[i] += X[i]*Y[i];
}

void smooth_l1_cpu(int n, float *pred, float *truth, float *delta, float *error)
{
    int i;
    for(i = 0; i < n; ++i){
        float diff = truth[i] - pred[i];
        float abs_val = fabs(diff);
        if(abs_val < 1) {
            error[i] = diff * diff;
            delta[i] = diff;
        }
        else {
            error[i] = 2*abs_val - 1;
            delta[i] = (diff < 0) ? 1 : -1;
        }
    }
}

void l1_cpu(int n, float *pred, float *truth, float *delta, float *error)
{
    int i;
    for(i = 0; i < n; ++i){
        float diff = truth[i] - pred[i];
        error[i] = fabs(diff);
        delta[i] = diff > 0 ? 1 : -1;
    }
}

void softmax_x_ent_cpu(int n, float *pred, float *truth, float *delta, float *error)
{
    int i;
    for(i = 0; i < n; ++i){
        float t = truth[i];
        float p = pred[i];
        error[i] = (t) ? -log(p) : 0;
        delta[i] = t-p;
    }
}

void logistic_x_ent_cpu(int n, float *pred, float *truth, float *delta, float *error)
{
    int i;
    for(i = 0; i < n; ++i){
        float t = truth[i];
        float p = pred[i];
        error[i] = -t*log(p) - (1-t)*log(1-p);
        delta[i] = t-p;
    }
}

void l2_cpu(int n, float *pred, float *truth, float *delta, float *error)
{
    int i;
    for(i = 0; i < n; ++i){
        float diff = truth[i] - pred[i];
        error[i] = diff * diff;
        delta[i] = diff;
    }
}

float dot_cpu(int N, float *X, int INCX, float *Y, int INCY)
{
    int i;
    float dot = 0;
    for(i = 0; i < N; ++i) dot += X[i*INCX] * Y[i*INCY];
    return dot;
}

void softmax(float *input, int n, float temp, int stride, float *output)
{
    int i;
    float sum = 0;
    float largest = -FLT_MAX;
    for(i = 0; i < n; ++i){
        if(input[i*stride] > largest) largest = input[i*stride];
    }
    for(i = 0; i < n; ++i){
        float e = exp(input[i*stride]/temp - largest/temp);
        sum += e;
        output[i*stride] = e;
    }
    for(i = 0; i < n; ++i){
        output[i*stride] /= sum;
    }
}


void softmax_cpu(float *input, int n, int batch, int batch_offset, int groups, int group_offset, int stride, float temp, float *output)
{
    int g, b;
    for(b = 0; b < batch; ++b){
        for(g = 0; g < groups; ++g){
            softmax(input + b*batch_offset + g*group_offset, n, temp, stride, output + b*batch_offset + g*group_offset);
        }
    }
}

void upsample_cpu(float *in, int w, int h, int c, int batch, int stride, int forward, float scale, float *out)
{
    int i, j, k, b;
    for(b = 0; b < batch; ++b){
        for(k = 0; k < c; ++k){
            for(j = 0; j < h*stride; ++j){
                for(i = 0; i < w*stride; ++i){
                    int in_index = b*w*h*c + k*w*h + (j/stride)*w + i/stride;
                    int out_index = b*w*h*c*stride*stride + k*w*h*stride*stride + j*w*stride + i;
                    if(forward) out[out_index] = scale*in[in_index];
                    else in[in_index] += scale*out[out_index];
                }
            }
        }
    }
}


