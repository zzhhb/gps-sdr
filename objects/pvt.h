/*! \file PVT.h
	Defines member functions of PVT class to do navigation
*/
/************************************************************************************************
Copyright 2008 Gregory W Heckler

This file is part of the GPS Software Defined Radio (GPS-SDR)

The GPS-SDR is free software; you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation; either version 2 of the
License, or (at your option) any later version.

The GPS-SDR is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License along with GPS-SDR; if not,
write to the: 

Free Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
************************************************************************************************/

#ifndef PVT_H
#define PVT_H

#include "includes.h"

/*! \ingroup CLASSES
 * 
 */
typedef class PVT
{

	private:
	
		pthread_t	thread;										//!< For the thread
		pthread_mutex_t	mutex;									//!< Protect the following variable
		PVT_2_Telem_S output;									//!< Structure to dump to telemetry
		PVT_2_SV_Select_S sv_select;							//!< Structure to dump to telemetry
		FIFO_2_Telem_S telem;									//!< Structure to dump to telemetry
		
		/* Satellite related stuff */
		Ephemeris_S		ephemerides[MAX_CHANNELS];				//!< Decoded ephemerides
		SV_Position_S	sv_positions[MAX_CHANNELS];				//!< Calculated SV positions
		Pseudorange_S	pseudoranges[MAX_CHANNELS];				//!< Pseudoranges
		Measurement_S	measurements[MAX_CHANNELS];				//!< Raw measurements

		int32 good_channels[MAX_CHANNELS];						//!< Is this a good channel (used to navigate)
		int32 master_iode[MAX_CHANNELS];						//!< Keep track of current IODE
		int32 master_sv[MAX_CHANNELS];							//!< Channel->SV map
		int32 sv_codes[MAX_CHANNELS];							//!< Error codes
		int32 doppler_suspect[MAX_CHANNELS][MAX_CHANNELS];		//!< For the cross-corr check

		/* Position and clock solutions */
		Nav_Solution_S	master_nav;								//!< Master nav sltn
		Nav_Solution_S	temp_nav;								//!< Temp nav sltn	
		Clock_S			master_clock;							//!< Master clock

		/* Matrices used in nav solution */
		double *alpha[MAX_CHANNELS];
		double alpha_2[4][4];
		double alpha_inv[4][4];
		double alpha_pinv[4][MAX_CHANNELS];
		
		double dircos[MAX_CHANNELS][4];
		double pseudorangeres[MAX_CHANNELS];
		double pseudorangerateres[MAX_CHANNELS];
		double dr[4];


	public:

		PVT(int32 _mode);
		~PVT();
		void Inport();							//!< Read from the corr_2_nav pipe
		void Start();							//!< Start the thread
		void Stop(); 							//!< Stop the thread
		void Navigate();						//!< main navigation task, call the following tabbed functions 
			void ProjectState();				//!< project state to current measurement epoch 
			void Update_Time();					//!< estimate GPS time at current tic 
			void Get_Ephemerides();				//!< get the current ephemeris from Ephemeris_Thread 
			void SV_TransitTime();				//!< Transit time to each SV 
			void SV_Positions();				//!< calculate SV Positions (for each channel with valid ephemeris) 
			void SV_Elevations();				//!< calculate SV Elevation angles for elevation mask 
			void SV_Correct();					//!< correct SV positions for transit time 
			void PseudoRange();					//!< calculate the pseudo ranges 
			void FormModel();					//!< form direction cosine matrix and prediction residuals 
			bool PreErrorCheck();				//!< check all SV's for bad measurements, etc
			void ErrorCheckCrossCorr();			//!< check for cross-correlation problem via ephemeris match			
			bool PostErrorCheck();				//!< check all SV's for bad measurements, etc
			bool Converged();					//!< declare convergence 
			void Residuals();					//!< compute resdiuals
			void PVT_Estimation();				//!< estimate PVT 
			void ClockUpdate();					//!< update the clock			 		
			void LatLong();						//!< convert ECEF coordinates to Lat,Long,Height 
			void DOP();							//!< calculate DOP terms 
			void ClockInit();					//!< initialize clock 
			void Raim();						//!< do a RAIM algorithm to look out for a bad SV
			
		void WritePVT();						//!< Write the PVT to disk for a later warm start
		void ReadPVT();							//!< Read  the PVT from disk for a later warm start

		Nav_Solution_S getNav(){return(master_nav);}
		Clock_S getClock(){return(master_clock);}
		
		void Lock();
		void Unlock();
		void Export();							//!< Export Navigator data to appropriate pipes 
		void Reset();							//!< Reset the Navigation solution to naught values 
		void Reset(int32 _chan);				//!< Reset individual channel
		double GPSTime();						//!< Calculate GPS time from PC's clock 
};


#endif


	
