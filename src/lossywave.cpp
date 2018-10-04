#include <lossywave.hpp>

#include <iostream>
#include <stdio.h>
#include <complex>
#include <time.h>
#include <omp.h>
#include <cstring>

// Wavelet functions
#include "wavelet.h"

// Sorting algorithms
#include <algorithm>
#include <gsl/gsl_sort.h>
#include "quick_sort.h"
//#include "bitonic_sort.h"

// Resolves Issue: https://stackoverflow.com/questions/30412951/unresolved-external-symbol-imp-fprintf-and-imp-iob-func-sdl2
// Only occurs when using pre-build GSL libraries from VS2013--
// Solution: Rebuild GSL with VS2015++
#if defined(_MSC_VER) && (_MSC_VER >= 1900)
FILE _iob[] = { *stdin, *stdout, *stderr };
extern "C" FILE * __cdecl __iob_func(void)
{
	return _iob;
}
#endif

#include "writer.h"
#include "reader.h"

#include "lz4.h"

namespace lossywave
{

	lossywave::lossywave()
	{
		params = NULL;
		pcnt = 10;
		lvl = 0;
		nthreads = 1;
		mode = 1; // 1d mode by default
	}

	lossywave::lossywave(int * inparams)
	{
		params = inparams;
		pcnt = params[11];
		lvl = params[12];
		nthreads = 1;
		
		// debug output
		std::cout << "Metadata for parameters:" << std::endl;
		std::cout << "Type: " << params[0] << " Level: " << params[1] << std::endl;
		std::cout << "Region: " << params[2] << " Padding: " << params[3] << std::endl;
		std::cout << "Local dims: " << params[4] << " " << params[5] << " " << params[6] << std::endl;
		std::cout << "Global dims: " << params[7] << " " << params[8] << " " << params[9] << std::endl;
		std::cout << "Data precision: " << params[10] << " bytes." << std::endl;
		if (params[2] >= 64)
			std::cout << "Compression: Enabled" << std::endl;
		if (params[2] >= 118)
			std::cout << "LZ4 Enabled" << std::endl;
		std::cout << "PcntThr: " << params[11] << " LvlThr: " << params[12] << std::endl;

		mode = 1; // 1d
		if (params[5] != 0) // 2d
			mode = 2;
		if (params[6] != 0) // 3d
			mode = 3;

	}

	size_t lossywave::compress(void * data, size_t dataType, void *&output)
	{
		gsl_wavelet *w;
		gsl_wavelet_workspace *work;
		size_t type = 303; // Cubic B-splines
		w = gsl_wavelet_alloc(gsl_wavelet_bspline, type);
		gsl_wavelet_print(w);

		std::cout << "Total threads in this machine: " << omp_get_max_threads() << std::endl;

		int pDims[3] = { params[4],params[5],params[6] };
		// Set to largest direction
		int val = pDims[0];
		if (val < pDims[1]) val = pDims[1];
		if (val < pDims[2]) val = pDims[2];

		size_t total = pDims[0] * pDims[1] * pDims[2];
		int stride = pDims[0];

		if (mode == 1)
			work = gsl_wavelet_workspace_alloc(total); //Normal during 1D decomp
		else
			work = gsl_wavelet_workspace_alloc(val); //Normal during 3d Decomp
		
		// copy data
		std::memcpy(output, data, total*params[10]);

		// wavelet transform the data
		std::cout << "Beginning Transform.. \n";
		
		clock_t start = clock();
		double start2 = omp_get_wtime();

		if (mode == 1)
		{}//wavelet_transform(w, static_cast<float*>(output), 1, total, gsl_wavelet_forward, work);
		else if (mode == 2)
			wavelet2d_nstransform(w, static_cast<float *>(output), stride, pDims[0], pDims[1], gsl_wavelet_forward, work);
		else if (mode == 3)
			gsl_wavelet3d_nstransform(w, static_cast<float *>(output), stride, pDims[0], pDims[1], pDims[2], gsl_wavelet_forward, work);

		start2 = omp_get_wtime() - start2;
		double secs = (clock() - start) / (double)1000;
		std::cout << std::endl << "Forward Transform done. Time elapsed = " << secs << " secs" << std::endl;
		std::cout << "Forward Transform done. OMP Time elapsed = " << start2 << " secs" << std::endl;

		gsl_wavelet_free(w);
		gsl_wavelet_workspace_free(work);


		float * woutput = static_cast<float *>(output);
		// sort data and threshold by pcnt
		if (lvl==0) //useLevel = false
		{
			std::cout << "Beginning sort\n";
			double * abscoeff = new double[total];
			size_t * p = new size_t[total];

			
			for (size_t i = 0; i < total; i++)
			{
				abscoeff[i] = fabs(woutput[i]);
			}

			start = clock();

			//gsl_sort_index (p, abscoeff, 1, total); // Serial, old and slow
			quick_sort_index(p, abscoeff, 1, total); // Fast, MPI Based sort
			secs = (clock() - start) / (double)1000;

			std::cout << "Ending parallel sort. Time elapsed = " << secs << " secs" << std::endl;

			delete[] abscoeff;

			//This begins %-based coefficient thresholding
			double num_coeff = (total*((double)pcnt / 100));
			std::cout << "Thresholding " << pcnt << "%, " << num_coeff << std::endl;
			start = clock();
			for (size_t i = 0; (i + num_coeff) < total; i++)
				woutput[p[i]] = 0;

			secs = (clock() - start) / (double)1000;
			std::cout << "Ending Threshold. Time elapsed = " << secs << " secs" << std::endl;
			delete[] p;
		}
		else
		{
			// Compute available levels
			int max_levels = 0;
			int curdim = pDims[0];
			while (curdim >= 2)
			{
				max_levels += 1;
				curdim /= 2;
			}

			// Calculate dimensions per each level
			int *** cdims = getCoeffDims(pDims[0], pDims[1], pDims[2]);

			std::cout << "Thresholding by level: " << lvl << " of " << max_levels << std::endl;
			start = clock();
			int curdims[3] = { pDims[0],pDims[1],pDims[2] };
			for (int lv = lossywave::lvl; lv < max_levels; lv++)
			{
				int lvlsz = cdims[lv][0][1] + 1; // xmax+1 assuming 3D cube
				lvlsz = lvlsz * lvlsz*lvlsz;
				for (int i = 1; i < 8; i++)
				{
					double * LLL;// = getCutout3d(woutput, cdims[lv][i], curdims);
					for (int index = 0; index < lvlsz; index++)
						LLL[index] = 0.0;

					//setCutout3d(woutput, LLL, cdims[lv][i], curdims);
					delete[] LLL;
				}
			}

			// Free cdims
			for (int i = max_levels - 1; i >= 0; i--)
			{
				for (int c = 0; c < 8; c++)
				{
					delete cdims[i][c];
				}
				delete cdims[i];
			}
			delete cdims;
			cdims = NULL;

			secs = (clock() - start) / (double)1000;
			std::cout << "Ending Threshold. Time elapsed = " << secs << " secs" << std::endl;
		}

		// requantize and encode coefficients
		//analyze(woutput); // Analysis on RLE efficiency

		// woutput is a static_cast<float *> of output, so be careful
		size_t comp_size = encode(woutput, output);
		//delete[] woutput;

		return comp_size;
	}


	size_t lossywave::decompress(void *data, void *&output)
	{

		float * out;
		//if (params[10] = 4)
		out = static_cast<float *>(output);

		// decode coefficients and requantize
		size_t decode_size = decode(data, out);

		// wavelet transform the data
		gsl_wavelet *w;
		gsl_wavelet_workspace *work;
		size_t type = 303; // Cubic B-splines
		w = gsl_wavelet_alloc(gsl_wavelet_bspline, type);

		std::cout << "Total threads in this machine: " << omp_get_max_threads() << std::endl;

		int pDims[3] = { params[4],params[5],params[6] };
		// Set to largest direction
		int val = pDims[0];
		if (val < pDims[1]) val = pDims[1];
		if (val < pDims[2]) val = pDims[2];

		size_t total = pDims[0] * pDims[1] * pDims[2];
		int stride = pDims[0];

		if (mode == 1)
			work = gsl_wavelet_workspace_alloc(total); //Normal during 1D decomp
		else
			work = gsl_wavelet_workspace_alloc(val); //Normal during 3d Decomp


		clock_t start = clock();
		double start2 = omp_get_wtime();

		if (mode == 1)
		{
		}//wavelet_transform(w, static_cast<float*>(output), 1, total, gsl_wavelet_forward, work);
		else if (mode == 2)
			wavelet2d_nstransform(w, out, stride, pDims[0], pDims[1], gsl_wavelet_backward, work);
		else if (mode == 3)
			gsl_wavelet3d_nstransform(w, out, stride, pDims[0], pDims[1], pDims[2], gsl_wavelet_backward, work);

		start2 = omp_get_wtime() - start2;
		double secs = (clock() - start) / (double)1000;
		std::cout << std::endl << "Inverse Transform done. Time elapsed = " << secs << " secs" << std::endl;
		std::cout << "Inverse Transform done. OMP Time elapsed = " << start2 << " secs" << std::endl;

		gsl_wavelet_free(w);
		gsl_wavelet_workspace_free(work);

		
		return decode_size;
	}

	template <typename T>
	void lossywave::analyze(T * data) 
	{
		// Experimental ----------------
		size_t total = params[4] * params[5] * params[6];

		// Count contiguous sectors of 0s for non-uniform data compression
		// Based on run-length encoding
		float sizeof_val = sizeof(data[0]);
		std::cout << "-------------------- RLE Analysis --------------------" << std::endl;
		std::cout << "Full coefficient file size (float): " << total * sizeof_val << " bytes or " << (total*sizeof_val) / (1024.0*1024.0) << " MB\n";
		double nonzero = 0.0;
		double zero_contiguous = 0.0;
		int times_contiguous = 0;


		int cont_block_cnt = 0; // DEBUG
		double largest_cont_block = 0; // DEBUG
		int cnt_exceed = 0;
		for (size_t i = 0; i < total; i++)
		{

			if (data[i] == 0)
			{
				cont_block_cnt = 0; // DEBUG
				times_contiguous++;
				while (data[i] == 0 && i < total)
				{
					zero_contiguous += sizeof_val;//8.0;
					i++;
					cont_block_cnt++; // DEBUG
				}
				if (cont_block_cnt > largest_cont_block) // DEBUG
					largest_cont_block = cont_block_cnt; // DEBUG
				while (cont_block_cnt > 65535)
				{
					cnt_exceed++;
					cont_block_cnt -= 65535;
				}
			}

			if (i < total)
				if (data[i] != 0)
					nonzero += sizeof_val; //8.0; //Double

		}

		std::cout << "Total number of zero bytes: " << zero_contiguous << " - Contiguous times: " << times_contiguous << "\n";
		std::cout << "Total non-zero bytes: " << nonzero << "\n";
		std::cout << "Total filesize non-compress (w header): " << nonzero + zero_contiguous + 32 << "\n";
		double comp_size = 32 + nonzero + 6.0*(double)(times_contiguous + cnt_exceed); // (4 bytes for signal + 2 bytes for value)
		std::cout << "Total filesize compressed (w header): " << comp_size << " bytes or " << comp_size / (1024.0*1024.0) << " MB\n";
		std::cout << "Compression ratio: " << (comp_size / (32 + (total*sizeof_val))) * 100 << "%  or " << (32 + (total*sizeof_val)) / comp_size << "\n";
		double overhead_mb = (comp_size / (1024.0*1024.0)) - ((32 + (total*sizeof_val)) / (1024.0*1024.0))*(pcnt / 100);
		std::cout << "Compression scheme overhead: " << overhead_mb << " MB or " << overhead_mb / (comp_size / (1024.0*1024.0))*100.0 << "%\n";
		std::cout << "DEBUG: Longest continuous block count: " << largest_cont_block << " - Times exceeded max unsigned short: " << cnt_exceed << "\n";
		std::cout << "------------------------------------------------------" << std::endl;
	}

	template <typename T>
	size_t lossywave::encode(T * in, void *& out)
	{
		size_t total = params[4] * params[5] * params[6];
		int cmpBytes = 0;

		// Check if we are using lz4 on coefficients
		if (params[2] >= 118)
		{
			std::cout << "Beginning LZ4 Routines...";
			// Optional: Perform floating point to integer requantization:
			// No Requantization if args[2]=128
			// When it's 125, it's 128-125 = 3. We divide by 1000
			// When it's 131, it's 128-131 = -3. Multibly by 1000
			double mod_bits = 1;
			size_t oval_sz = sizeof(T);

			if (params[2] > 128) // Multiply (for small dyn range data)
			{
				int cnt = params[2];
				while (cnt != 128)
				{
					mod_bits *= 10.0;
					cnt--;
				}
				oval_sz = sizeof(int);
				std::cout << "requantizing coefficients: 1*10^" << params[2] - 128 << std::endl;
			}
			if (params[2] < 128) // Divide (for high dyn range data)
			{
				int cnt = params[2];
				while (cnt != 128)
				{
					mod_bits /= 10.0;
					cnt++;
				}
				oval_sz = sizeof(int);
				std::cout << "requantizing coefficients: 1*10^-" << 128 - params[2] << std::endl;
			}


			// Target output variable (type)
			T dbl_val;
			//int dbl_val; // INT SUPPORT

			const size_t totBytes = total * oval_sz;
			size_t datBytes = totBytes;
			// WARNING: LZ4_MAX_INPUT_SIZE is about 2MB so suggest using a sliding buffer
			// Restriction inside of srcSize

			if (totBytes > LZ4_MAX_INPUT_SIZE)
			{
				std::cout << "Warning: Data to write is larger than supported by Lz4. Use rolling buffer!!\n";
				datBytes = LZ4_MAX_INPUT_SIZE;
			}

			LZ4_stream_t* lz4Stream = LZ4_createStream();
			const size_t cmpBufBytes = LZ4_COMPRESSBOUND(datBytes); //messageMaxBytes

			//Preallocate buffers
			char * inBuf = new char[datBytes];
			char * cmpBuf = new char[cmpBufBytes];

			// Use a sliding window approach and reinterpert type cast to char * array.
			// Use a ring buffer????

			size_t max_vals = datBytes / oval_sz;
			std::cout << " Encoding " << max_vals << " values.\n";

			size_t sk = 0;

			char * cval;

			// Copy data into a char buffer for lz4 comp
			// Check if we need to quantize first
			if (params[2] == 128)
			{
				// No quantization
				T dbl_val;
				for (size_t sz = 0; sz < max_vals; sz++)
				{
					dbl_val = in[sz];
					sk = sz * sizeof(dbl_val);
					cval = reinterpret_cast<char*>(&dbl_val);
					for (size_t id = 0; id < sizeof(dbl_val); id++)
					{
						inBuf[sk + id] = cval[id];
					}
					//inBuf[sk] = reinterpret_cast<char*>(&dbl_val), sizeof(dbl_val); //Insert T val into character buffer
					//memcpy more efficient?
				}
			}
			else if (oval_sz == 4 || oval_sz == 8) //maybe check sizeof(int)
			{
				// float -> int requantization
				int dbl_val; 
				for (size_t sz = 0; sz < max_vals; sz++)
				{
					//dbl_val = data[sz]*100000; // INT SUPPORT
					dbl_val = in[sz] * mod_bits; // INT SUPPORT

					sk = sz * sizeof(dbl_val);
					cval = reinterpret_cast<char*>(&dbl_val);
					for (size_t id = 0; id < sizeof(dbl_val); id++)
					{
						inBuf[sk + id] = cval[id];
					}
				}
			}
			else
			{
				std::cout << "ERROR: Invalid byte size!" << std::endl; return -1;
			}
			

			//const int cmpBytes = LZ4_compress_fast_continue(lz4Stream, inBuf, cmpBuf, datBytes, cmpBufBytes, 1); /rolling
			cmpBytes = LZ4_compress_default(inBuf, cmpBuf, datBytes, cmpBufBytes);

			std::cout << "LZ4: Encoded " << cmpBytes << " bytes..";

			// HACK: Free out buffer
			//if (out)
			//	delete[] out;

			// Reallocate/reduce the size of the output + size of header
			std::realloc(out, cmpBytes + 4);

			//Write out the buffer size first
			//write_uint16(FILE* fp, uint16_t i)
			memcpy((int*)out, &cmpBytes, sizeof(cmpBytes));

			// Transfer bytestream
			//out = cmpBuf;
			memcpy((char*)out + 4, cmpBuf, cmpBytes);

			delete[] inBuf;
			LZ4_freeStream(lz4Stream);
		}
		// Check if we are encoding coefficients with RLE
		else if (params[2] >= 64)
		{
			std::stringstream ss;
			std::cout << "Run-Length Encoding enabled...";
			//unsigned char signal = 0x24; //$ sign UTF-8
			char signal = '$';
			char signal2 = '@';
			char signal3 = '#';
			char signal4 = 'h';
			//std::cout << "Size of char=" << sizeof(signal) << " value " << signal << std::endl;
			unsigned short smax = 0;

			float wbytes = 0;
			int skips = 0;
			for (size_t sz = 0; sz < total; sz++)
			{

				if (in[sz] == 0)
				{
					skips++;
					int cont_block_cnt = 0;
					while (in[sz] == 0 && sz < total)
					{
						sz++;
						cont_block_cnt++; // DEBUG
					}
					// Encode the number of steps to skip
					while (cont_block_cnt > 65535) //Exceeded u_short_int
					{
						smax = 65535;
						ss.write(reinterpret_cast<char*>(&signal), sizeof(signal));
						ss.write(reinterpret_cast<char*>(&signal2), sizeof(signal2));
						ss.write(reinterpret_cast<char*>(&signal3), sizeof(signal3));
						ss.write(reinterpret_cast<char*>(&signal4), sizeof(signal4));
						ss.write(reinterpret_cast<char*>(&smax), sizeof(smax));
						skips++;
						cont_block_cnt -= 65535;
						wbytes += sizeof(signal) * 4 + sizeof(smax);
					}
					smax = cont_block_cnt;
					//std::cout << "\nSkipping at byte_loc: " << wbytes << " for " << smax << std::endl;
					ss.write(reinterpret_cast<char*>(&signal), sizeof(signal));
					ss.write(reinterpret_cast<char*>(&signal2), sizeof(signal2));
					ss.write(reinterpret_cast<char*>(&signal3), sizeof(signal3));
					ss.write(reinterpret_cast<char*>(&signal4), sizeof(signal4));
					ss.write(reinterpret_cast<char*>(&smax), sizeof(smax));
					wbytes += sizeof(signal) * 4 + sizeof(smax);
				}

				T dbl_val = in[sz];
				ss.write(reinterpret_cast<char*>(&dbl_val), sizeof(dbl_val));
				wbytes += sizeof(dbl_val);
			}
			//std::copy(ss.str().c_str(), ss.str().c_str() + ss.str().length() + 1, out);
			std::cout << "...Coefficient's saved\n";
			cmpBytes = wbytes + 32; // Data stream + header
			std::cout << "Bytes written (w header): " << cmpBytes << std::endl;
			std::cout << "Contiguous skips: " << skips << std::endl;
			
			//Set stringstream to void *& out
			memcpy(out, ss.str().c_str(), cmpBytes - 32);
		}
		// Just write coefficients to storage with zeros intact
		else
		{
			std::stringstream ss;
			std::cout << "No Encoding...";
			for (size_t sz = 0; sz < total; sz++)
			{
				T dbl_val = in[sz];
				ss.write(reinterpret_cast<char*>(&dbl_val), sizeof(T));
			}
			cmpBytes = total * sizeof(T);
			std::cout << "Coefficient's saved\n";
			//std::copy(ss.str().c_str(), ss.str().c_str() + ss.str().length() + 1, out);

			//Set stringstream to void *& out
			memcpy(out, ss.str().c_str(), cmpBytes);
		}

		return cmpBytes;
	}

	template <typename T>
	size_t lossywave::decode(void * in, T *& out)
	{
		size_t total = params[4] * params[5] * params[6];
		int dcmpBytes = 0;

		
		// static cast out to type T here to write actual values
		T * output = static_cast<T *>(out);
		
		if (sizeof(T) != params[10])
			std::cout << "Warning: Header Data Precision " << params[10] << " does not match target " << sizeof(T) << std::endl;

		
		//std::cout << "Opening "<< filename << std::endl;
		//std::cout.precision(20);
		if (params[2] >= 118)
		{
			
			//char strm1[4];
			// Begin reading the number of bytes
			//f.read(reinterpret_cast<char*>(&cmpBytes), sizeof(cmpBytes));
			char * buff = static_cast<char *>(in);

			char ctoi[4];
			for (int i = 0; i < 4; i++)
				ctoi[i] = buff[i];

			int cmpBytes = *reinterpret_cast<int*>(ctoi);

			std::cout << "LZ4: Reading in " << cmpBytes << " bytes..." << std::endl;

			// copy to stringstream
			//std::stringstream ss;
			//ss.write((const char*)in + 4, cmpBytes);

			// Optional: Reverse integer to fp requantization:
			// No Requantization if params[2]=128
			// When it's 125, it's 128-125 = 3. We multiply by 1000
			// When it's 131, it's 128-131 = -3. divide by 1000
			double mod_bits = 1;
			size_t oval_sz = sizeof(T);

			if (params[2] > 128) // Divide (undo requant)
			{
				int cnt = params[2];
				while (cnt != 128)
				{
					mod_bits /= 10.0;
					cnt--;
				}
				oval_sz = sizeof(int);
				std::cout << "requantizing coefficients: 1*10^-" << params[2] - 128 << std::endl;
			}
			if (params[2] < 128) // Multiply (undo requant)
			{
				int cnt = params[2];
				while (cnt != 128)
				{
					mod_bits *= 10.0;
					cnt++;
				}
				oval_sz = sizeof(int);
				std::cout << "requantizing coefficients: 1*10^" << 128 - params[2] << std::endl;
			}

			// Define the temporary type (requant test)
			//T dbl_val;
			//int dbl_val; // INT SUPPORT

			//char * cmpBuf = new char[cmpBytes];
			char * cmpBuf = buff + 4;
			//memcpy(cmpBuf, buff + 4, cmpBytes);
			char * outBuf = new char[total*oval_sz];

			//ss.read(cmpBuf, cmpBytes);

			std::cout << "Read in bytes..Decompressing LZ4..";

			// Allocate temporary memory
			dcmpBytes = LZ4_decompress_safe(cmpBuf, outBuf, cmpBytes, total * oval_sz);
			//delete[] cmpBuf;

			std::cout << "...Done!" << std::endl;

			// Copy byte memory into output data array
			size_t sk = 0;

			// Check if we need to requantize values
			if (params[2] == 128)
			{
				// No Requantization
				char * cval;
				T dbl_val=0;

				cval = reinterpret_cast<char*>(&dbl_val);

				for (size_t sz = 0; sz < total; sz++)
				{
					for (size_t ind = 0; ind < sizeof(dbl_val); ind++)
					{
						cval[ind] = outBuf[(sz * sizeof(dbl_val)) + ind];
					}
					output[sz] = dbl_val;
				}
			}
			else if (oval_sz == 8) // Type DOUBLE //TODO: This means no requantization
			{
				char conc[8];
				T dbl_val;

				for (size_t sz = 0; sz < total; sz++)
				{
					for (size_t ind = 0; ind < sizeof(dbl_val); ind++)
					{
						conc[ind] = outBuf[(sz * sizeof(dbl_val)) + ind];
					}
					dbl_val = *reinterpret_cast<T*>(conc);

					output[sz] = dbl_val;


					//outBuf[sz * sizeof(T)];

					//dbl_val = *reinterpret_cast<T*>(outBuf[sz * sizeof(dbl_val)]);
					//std::cout << " v: " << dbl_val << " ";
					//data[sz] = dbl_val;
					//dbl_val = data[sz];
					//sk = sz * sizeof(T);
					//char * cval = reinterpret_cast<char*>(&dbl_val);

					//for (size_t id = 0; id < sizeof(dbl_val); id++)
					//{
					//	inBuf[sk + id] = cval[id];
					//}
					//delete[] cval;
					//inBuf[sk] = reinterpret_cast<char*>(&dbl_val), sizeof(dbl_val); //Insert T val into character buffer
					//memcpy?
				}
			}
			else if (oval_sz == 4) // Type FLOAT input, may be float/double output
			{
				char conc[4]; // INT SUPPORT
				if (params[10] == 8) // Requantize ints to floats
				{
					int dbl_val;
					for (size_t sz = 0; sz < total; sz++)
					{
						for (size_t ind = 0; ind < 4; ind++)
						{
							conc[ind] = outBuf[(sz * sizeof(dbl_val)) + ind];
						}
						dbl_val = *reinterpret_cast<int*>(conc); // INT SUPPORT
						output[sz] = (T)dbl_val * mod_bits; // INT SUPPORT
					}
				}
				else if (params[10] == 4)
				{
					T dbl_val;
					for (size_t sz = 0; sz < total; sz++)
					{
						for (size_t ind = 0; ind < 4; ind++)
						{
							conc[ind] = outBuf[(sz * sizeof(dbl_val)) + ind];
						}
						output[sz] = *reinterpret_cast<T*>(conc);
					}
				}
				else
				{


				}

			}
			else if (params[10] == 2)
			{

			}
			else
			{
				std::cout << " Type mismatch. Not implemented!" << std::endl;
				return 0;
			}

			delete[] outBuf;
			std::cout << "..LZ4 finished data copy!\n";

		}
		/*// Undo RL Encoding
		else if (params[2] >= 64)
		{

			int skip_cnt = 0;
			size_t val_read = 0;
			double rbytes = 0;
			std::cout << "Encoding enabled...";
			//unsigned char signal = 0x24; //$ sign UTF-8
			char signal = '$';
			char signal2 = '@';
			char signal3 = '#';
			char signal4 = 'h';

			char strm1[4];
			char strm2[4]; //sizeof(var)-1
			unsigned short skip = 0;
			for (size_t sz = 0; sz < total; sz++)
			{
				f.read(strm1, sizeof(signal) * 4);
				rbytes += sizeof(signal) * 4; //debug

				if (strm1[0] == signal && strm1[1] == signal2 && strm1[2] == signal3 && strm1[3] == signal4) //$ is unsigned, but read as a signed char. Conflict?
				//if(*reinterpret_cast<int*>(strm1) == signal)
				{
					skip_cnt++;

					f.read(reinterpret_cast<char*>(&skip), sizeof(skip));
					rbytes += sizeof(skip); //debug
					//if (skip_cnt < 10)
					//cout << "\nFound skip flag ["<<strm1[0]<<"]["<<strm1[1]<<"] at "<< rbytes <<" for size: " << skip << endl;
					if (skip > total)
					{
						std::cout << "ERROR: Data read error: Skip size > total size\n";
						exit(0);
					}
					//cout << "Found skip flag ["<<*reinterpret_cast<int*>(strm1)<<"] at "<< rbytes <<" for size: " << skip << endl;

					while (skip > 1 && sz < total)
					{
						out[sz] = 0.0;
						sz++;
						skip = skip - 1;
					}
				}
				else
				{

					if (params[10] == 8) // Type DOUBLE
					{
						// Read second half of the data
						f.read(strm2, 4);
						rbytes += 4;

						char conc[8];
						//Combine two sections
						//memcpy(&conc[0], strm1, 2);
						//memcpy(&conc[3], strm2, 6);
						std::copy(strm1, strm1 + 4, conc);
						std::copy(strm2, strm2 + 4, conc + 4);


						//double dbl_val = *reinterpret_cast<double*>(conc);
						//std::cout << "Read double: " << out[sz] << std::endl;
						//out[sz] = dbl_val;

						out[sz] = *reinterpret_cast<T*>(conc);
						val_read += 1;
					}
					else if (params[10] == 4) // Type FLOAT
					{
						// 4 bytes already in memory, just convert
						char conc[4];
						std::copy(strm1, strm1 + 4, conc);
						out[sz] = *reinterpret_cast<T*>(conc);
						val_read += 1;
					}
					//else if(params[10]==2) // Type SHORT
					//{
//						// 4 bytes already in memory, split into 2 values
						//char conc[2];
						//std::copy(strm1,strm1+2,conc);
						//out[sz] =  *reinterpret_cast<T*>(conc);
						//val_read+=1;
						//sz++;

						//TODO: These two bytes may be the beginning of the signal.
						//		Disable control block until above is fixed
						//std::copy(strm1+2,strm1+4,conc);
						//out[sz] =  *reinterpret_cast<T*>(conc);
						//val_read+=1;
					//}
					else
					{
						std::cout << "ERROR: Data encoded in unsupported type. Bytes per value: " << params[10] << std::endl;
						delete[] out;
						return NULL;
					}
				}


				//if (out[sz] == 0)

			}
			std::cout << "...Coefficient's loaded\n";

			std::cout << "Bytes Read (w header): " << rbytes + 32 << endl;
			std::cout << "Contiguous times: " << skip_cnt << std::endl;
			std::cout << "Values Read: " << val_read << std::endl;

		}
		else
		{
			T dbl_val = 0.0;
			for (size_t sz = 0; sz < total; sz++)
			{
				f.read(reinterpret_cast<char*>(&dbl_val), sizeof(T));
				out[sz] = dbl_val;
			}
			//std::cout<<"Coefficient's loaded\n";
		}*/

		return dcmpBytes;
	}
}