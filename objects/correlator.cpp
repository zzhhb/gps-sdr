/*! \file Correlator.cpp
	Implements member functions of Correlator class.
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

#include "correlator.h"

/* Be sure to init static variable prior to use by actual objects */
CPX *Correlator::sine_table = new CPX[(2*CARRIER_BINS+1)*2*SAMPS_MS];
CPX **Correlator::sine_rows = new CPX*[2*CARRIER_BINS+1];
MIX *Correlator::main_code_table = new MIX[NUM_CODES*(2*CODE_BINS+1)*2*SAMPS_MS];
MIX **Correlator::main_code_rows = new MIX*[NUM_CODES*(2*CODE_BINS+1)];

/*----------------------------------------------------------------------------------------------*/
void *Correlator_Thread(void *_arg)
{

	Correlator *aCorrelator = pCorrelators[*(int32 *)_arg];

	while(grun)
	{
		aCorrelator->Inport();
		aCorrelator->Correlate();
	}

	pthread_exit(0);

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void Correlator::Start()
{
	pthread_attr_t tattr;
	sched_param param;
	int32 ret;

	/* Unitialized with default attributes */
	ret = pthread_attr_init(&tattr);

	/*Ssafe to get existing scheduling param */
	ret = pthread_attr_getschedparam(&tattr, &param);

	/* Set the priority; others are unchanged */
	param.sched_priority = CORR_PRIORITY;

	/* Setting the new scheduling param */
	ret = pthread_attr_setschedparam(&tattr, &param);
	ret = pthread_attr_setschedpolicy(&tattr, SCHED_FIFO);

	/* With new priority specified */
	pthread_create(&thread, NULL, Correlator_Thread, &chan);

	if(gopt.verbose)
		printf("Started correlator %d\n",chan);
}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void Correlator::Stop()
{
	pthread_cancel(thread);
	pthread_join(thread, NULL);

	if(gopt.verbose)
		printf("Stopped correlator %d\n",chan);
}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
Correlator::Correlator(int32 _chan)
{

	int32 lcv;

	chan = _chan;
	packet_count = 0;
	state.active = 0;
	aChannel = pChannels[chan];

	/* Malloc memory for local code vector */
	code_table = new MIX[(2*CODE_BINS+1)*2*SAMPS_MS];
	code_rows = new MIX*[2*CODE_BINS+1];
	for(lcv = 0; lcv < 2*CODE_BINS+1; lcv++)
		code_rows[lcv] = &code_table[lcv*2*SAMPS_MS];

	if(chan == 0)
	{
		/* Get the pointers */
		for(lcv = 0; lcv < 2*CARRIER_BINS+1; lcv++)
			sine_rows[lcv] = &sine_table[lcv*2*SAMPS_MS];

		/* Create the wipeoff */
		for(lcv = -CARRIER_BINS; lcv <= CARRIER_BINS; lcv++)
			sine_gen(sine_rows[lcv+CARRIER_BINS], -IF_FREQUENCY-(float)lcv*CARRIER_SPACING, SAMPLE_FREQUENCY, 2*SAMPS_MS);

		for(lcv = 0; lcv < (2*CODE_BINS+1)*NUM_CODES; lcv++)
			main_code_rows[lcv] = &main_code_table[lcv*2*SAMPS_MS];

		SamplePRN();
	}

	if(gopt.verbose)
		printf("Creating Correlator %d\n",chan);

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
Correlator::~Correlator()
{

	delete [] code_table;
	delete [] code_rows;

	if(chan == 0)
	{
		delete [] sine_table;
		delete [] sine_rows;
		delete [] main_code_table;
		delete [] main_code_rows;
	}

	if(gopt.verbose)
		printf("Destructing Correlator %d\n",chan);
}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void Correlator::Inport()
{
	int32 bread;
	int32 lcv;
	int32 last;
	Acq_Request_S temp;
	ms_packet *p = NULL;

	/* Wait for a command to start a new channel */
	if(state.active == 0)
	{
		bread = read(Trak_2_Corr_P[chan][READ], &result, sizeof(Acq_Result_S));
		if(bread == sizeof(Acq_Result_S))
		{
			InitCorrelator();

			switch(result.type)
			{
				case ACQ_STRONG:
					aChannel->Start(result.sv, result, 1);
					break;
				case ACQ_MEDIUM:
					aChannel->Start(result.sv, result, 4);
					break;
				case ACQ_WEAK:
					aChannel->Start(result.sv, result, 4);
					break;
			}

			/* Set correlator status to active */
			pthread_mutex_lock(&mInterrupt);
			gInterrupt[chan] = 1;
			pthread_mutex_unlock(&mInterrupt);
		}
	}

	/* Should do this ONCE with built in blocking! */
	last = packet.count;
	pFIFO->Dequeue(chan, &packet);
//	pFIFO->Wait(chan); //Pend until everyone has called dequeue

	while(packet.count == last)
	{
		usleep(gopt.corr_sleep);
		pFIFO->Dequeue(chan, &packet);
	}

	packet_count++;

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void Correlator::Correlate()
{
	Correlation_S *c;
	CPX *if_data;
	int32 leftover;

	if(packet.measurement)
		TakeMeasurement();

	if(state.active)
	{
		if_data = &packet.data[0];
		c = &corr;
		leftover = SAMPS_MS;

		/* If the rollover occurs in this packet */
		if(state.rollover <= SAMPS_MS)
		{
			/* Do the actual accumulation */
			Accum(c, if_data, state.rollover);

			/* Remaining number of samples to be processed in this ms packet of data */
			leftover = SAMPS_MS - state.rollover;
			if_data += state.rollover;

			/* Update the code/carrier phase etc */
			UpdateState(state.rollover);

			/* Dump the accumulation */
			DumpAccum(c);

			if(state.active == 0)
				return;

			/* Now process remaining segment of IF data  */
			if(state.rollover <= leftover) /* Rollover occurs in THIS packet of data */
			{
				/* Do the actual accumulation */
				Accum(c, if_data, state.rollover);

				/* Remaining number of samples to be processed in this ms packet of data */
				leftover -= state.rollover;
				if_data += state.rollover;

				/* Update the code/carrier phase etc */
				UpdateState(state.rollover);

				/* Dump the accumulation */
				DumpAccum(c);
				if(state.active == 0)
					return;

				/* Do the actual accumulation */
				Accum(c, if_data, leftover);

				/* Update the code/carrier phase etc */
				UpdateState(leftover);

			}
			else /* Rollover occurs in NEXT packet of data */
			{
				/* Do the actual accumulation */
				Accum(c, if_data, leftover);

				/* Update the code/carrier phase */
				UpdateState(leftover);
			}

		}
		else /* Just accumulate, no dumping */
		{
			/* Do the actual accumulation */
			Accum(c, if_data, SAMPS_MS);

			/* Update the code/carrier phase */
			UpdateState(SAMPS_MS);
		}
	}

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void Correlator::TakeMeasurement()
{

	int32 lcv, tic;
	int32 n_dp, n_p, n_c;
	Measurement_S *pmeas;

	tic = packet.measurement;

	/* Step 1, copy in measurement from ICP_TICS ago */
	memcpy(&meas, &meas_buff[(tic - ICP_TICS + TICS_PER_SECOND) % TICS_PER_SECOND], sizeof(Measurement_S));

	/* Get carrier phase prev from 2*ICP_TICKS ago */
	meas.carrier_phase_prev = meas_buff[(tic - 2*ICP_TICS + TICS_PER_SECOND) % TICS_PER_SECOND].carrier_phase;

	/* Get current carrier phase */
	meas.carrier_phase = state.carrier_phase;

	/* Store rest of measurement in buffer to do the delay */
	pmeas = &meas_buff[tic % (TICS_PER_SECOND)];
	pmeas->chan				 = chan;
	pmeas->code_phase 		 = state.code_phase;
	pmeas->code_phase_mod 	 = state.code_phase_mod;
	pmeas->carrier_phase 	 = state.carrier_phase;
	pmeas->carrier_phase_mod = state.carrier_phase_mod;
	pmeas->code_nco 		 = state.code_nco;
	pmeas->carrier_nco 		 = state.carrier_nco;
	pmeas->_1ms_epoch 		 = state._1ms_epoch;
	pmeas->_20ms_epoch 		 = state._20ms_epoch;
	pmeas->_z_count 		 = state._z_count;
	pmeas->sv				 = state.sv;
	pmeas->count			 = packet.count;
	pmeas->navigate			 = state.navigate;

	n_dp = meas_buff[(tic - 2*ICP_TICS + TICS_PER_SECOND) % TICS_PER_SECOND].navigate;
	n_p = meas_buff[(tic - ICP_TICS + TICS_PER_SECOND) % TICS_PER_SECOND].navigate;
	n_c = meas_buff[tic % TICS_PER_SECOND].navigate;

	/* Mark navigate only if all 3 are navigate */
	meas.navigate = n_dp && n_p && n_c;

	/* Write over measurement */
	write(Corr_2_PVT_P[chan][WRITE], &meas, sizeof(Measurement_S));

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void Correlator::UpdateState(int32 samps)
{



	/* Update phase states */
	state.code_phase 	 		+= samps*state.code_nco/SAMPLE_FREQUENCY;
	state.carrier_phase			+= samps*state.carrier_nco/SAMPLE_FREQUENCY;

	/* Do this to catch code epoch rollovers */
	state.code_phase_mod		+= samps*state.code_nco/SAMPLE_FREQUENCY;
	state.carrier_phase_mod		+= samps*state.carrier_nco/SAMPLE_FREQUENCY;

	/* A double rollover MIGHT occur? */
	if(state.code_phase_mod >= 2.0*(double)CODE_CHIPS)
	{
		state._1ms_epoch += 2;
		if(state._1ms_epoch >= 20)
		{
			state._1ms_epoch %= 20;
			state._20ms_epoch++;

			if(state._20ms_epoch >= 300)
			{
				state._20ms_epoch = 0;
				state._z_count += 6;

				if(state._z_count > SECONDS_IN_WEEK)
					state._z_count = 0;
			}
		}
	} /* If the C/A code rolls over then the 1ms and 20ms counters need incremented */
	else if(state.code_phase_mod >= (double)CODE_CHIPS)
	{
		state._1ms_epoch++;
		if(state._1ms_epoch >= 20)
		{
			state._1ms_epoch %= 20;
			state._20ms_epoch++;

			if(state._20ms_epoch >= 300)
			{
				state._20ms_epoch = 0;
				state._z_count += 6;

				if(state._z_count > SECONDS_IN_WEEK)
					state._z_count = 0;
			}
		}
	}

	/* Update partial phase states */
	state.carrier_phase_mod  	 = fmod(state.carrier_phase_mod, 1.0);
	state.code_phase_mod	  	 = fmod(state.code_phase_mod, CODE_CHIPS);

	state.rollover -= samps;

	/* Update pointers to presampled Doppler and PRN vectors */
	state.psine    += samps;
	state.pcode[0] += samps;
	state.pcode[1] += samps;
	state.pcode[2] += samps;
	state.scount   += samps;


}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void Correlator::Accum(Correlation_S *c, CPX *data, int32 samps)
{

	CPX_ACCUM EPL[3];

	//SineGen(samps);
	//state.psine = sine_rows[chan];

	/* First do the wipeoff */
	sse_cmulsc(data, state.psine, scratch, samps, 14);

	/* Now do the accumulation */
	//sse_prn_accum(scratch, state.pcode[0], state.pcode[1], state.pcode[2], samps, &EPL[0]);
	sse_prn_accum_new(scratch, state.pcode[0], state.pcode[1], state.pcode[2], samps, &EPL[0]);

	c->I[0] += (int32) EPL[0].i;
	c->I[1] += (int32) EPL[1].i;
	c->I[2] += (int32) EPL[2].i;

	c->Q[0] += (int32) EPL[0].q;
	c->Q[1] += (int32) EPL[1].q;
	c->Q[2] += (int32) EPL[2].q;

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void Correlator::DumpAccum(Correlation_S *c)
{
	float f1, f2, fix, ang;
	float code_phase;
	int32 bin, offset, lcv, bread;
	float sang, cang, tI, tQ;

	/* First rotate correlation based on nco frequency and actually frequency used for correlation */
	f1 = ((state.sbin - CARRIER_BINS) * CARRIER_SPACING) + IF_FREQUENCY;
	f2 = state.carrier_nco;
	fix = (float)PI*(f2-f1)*(float)state.scount/(float)SAMPLE_FREQUENCY;

	ang = state.carrier_phase_prev*(float)TWO_PI + fix;
	ang = -ang; cang = cos(ang); sang = sin(ang);

	state.carrier_phase_prev = state.carrier_phase_mod;

	tI = c->I[0];	tQ = c->Q[0];
	c->I[0] = (int32)floor(cang*tI - sang*tQ);
	c->Q[0] = (int32)floor(sang*tI + cang*tQ);

	tI = c->I[1];	tQ = c->Q[1];
	c->I[1] = (int32)floor(cang*tI - sang*tQ);
	c->Q[1] = (int32)floor(sang*tI + cang*tQ);

	tI = c->I[2];	tQ = c->Q[2];
	c->I[2] = (int32)floor(cang*tI - sang*tQ);
	c->Q[2] = (int32)floor(sang*tI + cang*tQ);

	/* Get the feedback */
	aChannel->Accum(c, &feedback);

	 /* Apply feedback */
	ProcessFeedback(&feedback);

	/* Is this needed? */
	state.count++;

	/* Now clear out accumulation */
	c->I[0] = c->I[1] = c->I[2] = 0;
	c->Q[0] = c->Q[1] = c->Q[2] = 0;

	/* Calculate when next rollover occurs (in samples) */
	state.rollover = (int32) ceil(((double)CODE_CHIPS - state.code_phase_mod)*SAMPLE_FREQUENCY/state.code_nco);

	bin = (int32) floor((state.code_phase_mod + 0.5)*CODE_BINS + 0.5) + CODE_BINS/2;
	if(bin < 0)	bin = 0; if(bin > 2*CODE_BINS) bin = 2*CODE_BINS;
	state.pcode[0] = code_rows[bin];
	state.cbin[0] = bin;

	bin = (int32) floor((state.code_phase_mod + 0.0)*CODE_BINS + 0.5) + CODE_BINS/2;
	if(bin < 0)	bin = 0; if(bin > 2*CODE_BINS) bin = 2*CODE_BINS;
	state.pcode[1] = code_rows[bin];
	state.cbin[1] = bin;

	bin = (int32) floor((state.code_phase_mod - 0.5)*CODE_BINS + 0.5) + CODE_BINS/2;
	if(bin < 0)	bin = 0; if(bin > 2*CODE_BINS) bin = 2*CODE_BINS;
	state.pcode[2] = code_rows[bin];
	state.cbin[2] = bin;

	/* Update pointer to pre-sampled sine vector */
	bin = (int32) floor((state.carrier_nco - IF_FREQUENCY)/CARRIER_SPACING + 0.5) + CARRIER_BINS;

	/* Catch errors if Doppler goes out of range */
	if(bin < 0)	bin = 0; if(bin > 2*CARRIER_BINS) bin = 2*CARRIER_BINS;
	state.psine = sine_rows[bin];
	state.sbin = bin;

	/* Remember to nuke this! */
	state.scount = 0;

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void Correlator::ProcessFeedback(NCO_Command_S *f)
{

	state.carrier_nco  	= f->carrier_nco;
	state.code_nco 	   	= f->code_nco;
	state.navigate		= f->navigate;

	nco_phase_inc = (uint32)floor((double)state.carrier_nco*(double)2.097152000000000e+03);

	if(f->reset_1ms)
		state._1ms_epoch = 0;

	if(f->reset_20ms)
		state._20ms_epoch = 60;

	if(f->set_z_count)
		state._z_count = f->z_count;

	/* Update correlator state */
	if(f->kill)
	{
		/* Clear out some buffers */
		memset(&state, 		0x0, sizeof(Correlator_State_S));
		memset(&meas, 		0x0, sizeof(Measurement_S));
		memset(&meas_buff, 	0x0, TICS_PER_SECOND*sizeof(Measurement_S));

		/* Set correlator status to inactive */
		pthread_mutex_lock(&mInterrupt);
		gInterrupt[chan] = 0;
		pthread_mutex_unlock(&mInterrupt);
	}

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void Correlator::Export()
{


}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void Correlator::SineGen(int32 _samps)
{

	int32 lcv;
	uint32 index;

	for(lcv = 0; lcv < _samps; lcv++)
	{
		index = nco_phase >> 21;
		sine_rows[chan][lcv].i = lookup[index].i;
		sine_rows[chan][lcv].q = lookup[index].q;
		nco_phase += nco_phase_inc;
	}

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void Correlator::SamplePRN()
{
	MIX *row;
	int32 lcv, lcv2, sv, k;
	int32 index;
	float phase_step, phase;

	k = 0;

	for(sv = 0; sv < NUM_CODES; sv++)
	{

		code_gen(&scratch[0], sv);

		for(lcv = 0; lcv < 2*CODE_BINS+1; lcv++)
		{

			row = main_code_rows[k];
			k++;

			phase = -0.5 + (float)lcv/(float)CODE_BINS;
			phase_step = CODE_RATE/SAMPLE_FREQUENCY;

			for(lcv2 = 0; lcv2 < 2*SAMPS_MS; lcv2++)
			{
				index  = (int32)floor(phase + CODE_CHIPS) % CODE_CHIPS;

				if(scratch[index].i)
					row[lcv2].i = row[lcv2].ni = 0x0001; /* Map 1 to 0x0000, and 0 to 0xffff for SIMD code */
				else
					row[lcv2].i = row[lcv2].ni = 0xffff;

				row[lcv2].q = row[lcv2].nq = 0x0;

				phase += phase_step;
			}
		}

	}

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void Correlator::GetPRN(int32 _sv)
{

	int32 lcv;

	if(_sv >= 0 && _sv <= 31)
	{
		for(lcv = 0; lcv < (2*CODE_BINS+1); lcv++)
		{
			memcpy(code_rows[lcv], main_code_rows[lcv + _sv*(2*CODE_BINS+1)], 2*SAMPS_MS*sizeof(CPX));
		}
	}

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void Correlator::InitCorrelator()
{
	double code_phase;
	double dt;
	int32 bin;
	int32 inc;

	/* Update delay based on current packet count */
	dt = (double)packet.count - (double)result.count;
	dt *= (double).001;
	dt *= (double)result.doppler*(double)CODE_RATE/(double)L1;
	result.delay += (double)CODE_CHIPS + dt;
	result.delay = fmod(result.delay,(double) CODE_CHIPS);

	state.sv					= result.sv;
	state.navigate				= false;
	state.active 				= 1;
	state.count					= 0;
	state.scount				= 0;
	state.code_phase 			= result.delay;
	state.code_phase_mod 		= result.delay;
	state.carrier_phase 		= 0;
	state.carrier_phase_mod 	= 0;
	state.code_nco				= CODE_RATE + result.doppler*CODE_RATE/L1;
	state.carrier_nco			= IF_FREQUENCY + result.doppler;
	state._1ms_epoch 			= 0;
	state._20ms_epoch			= 0;
	state.rollover 				= (int32) ceil(((double)CODE_CHIPS - state.code_phase)*SAMPLE_FREQUENCY/state.code_nco); /* Calculate rollover point */

	GetPRN(state.sv);

	nco_phase_inc = (uint32)floor((double)state.carrier_nco*(double)2.097152000000000e+03);
	nco_phase = 0;

	//inc = (int32)floor(result.delay*2048.0/1023.0);

	/* Initialize the code bin pointers */
	bin = (int32) floor((state.code_phase_mod + 0.5)*CODE_BINS + 0.5) + CODE_BINS/2;
	if(bin < 0)	bin = 0; if(bin > 2*CODE_BINS) bin = 2*CODE_BINS;
	state.pcode[0] = code_rows[bin];
	state.pcode[0] += inc;
	state.cbin[0] = bin;

	bin = (int32) floor((state.code_phase_mod + 0.0)*CODE_BINS + 0.5) + CODE_BINS/2;
	if(bin < 0)	bin = 0; if(bin > 2*CODE_BINS) bin = 2*CODE_BINS;
	state.pcode[1] = code_rows[bin];
	state.pcode[1] += inc;
	state.cbin[1] = bin;

	bin = (int32) floor((state.code_phase_mod - 0.5)*CODE_BINS + 0.5) + CODE_BINS/2;
	if(bin < 0)	bin = 0; if(bin > 2*CODE_BINS) bin = 2*CODE_BINS;
	state.pcode[2] = code_rows[bin];
	state.pcode[2] += inc;
	state.cbin[2] = bin;

	/* Update pointer to pre-sampled sine vector */
	bin = (int32) floor((state.carrier_nco - IF_FREQUENCY)/CARRIER_SPACING + 0.5) + CARRIER_BINS;

	/* Catch errors if Doppler goes out of range */
	if(bin < 0)	bin = 0; if(bin > 2*CARRIER_BINS) bin = 2*CARRIER_BINS;
	state.psine = sine_rows[bin];
	state.sbin = bin;

	//printf("Correlator initialized %d,%d,%f,%f,%f,%d,%d\n",chan,result.sv,state.carrier_nco,state.code_nco,state.code_phase,packet.count,result.count);

}
/*----------------------------------------------------------------------------------------------*/




