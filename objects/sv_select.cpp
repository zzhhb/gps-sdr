/*! \file SV_Select.cpp
	Implements member functions of SV_Select class.
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

#include "sv_select.h"

/*----------------------------------------------------------------------------------------------*/
void *SV_Select_Thread(void *_arg)
{

	SV_Select *aSV_Select = pSV_Select;

	while(grun)
	{
		aSV_Select->Inport();
		aSV_Select->Acquire();
		aSV_Select->Export();
		usleep(100000);
	}

	pthread_exit(0);

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void SV_Select::Start()
{
	pthread_attr_t tattr;
	pthread_t tid;
	int32 ret;
	sched_param param;

	/* Unitialized with default attributes */
	ret = pthread_attr_init (&tattr);

	/*Ssafe to get existing scheduling param */
	ret = pthread_attr_getschedparam (&tattr, &param);

	/* Set the priority; others are unchanged */
	param.sched_priority = TRAK_PRIORITY;

	/* Setting the new scheduling param */
	ret = pthread_attr_setschedparam(&tattr, &param);
	ret = pthread_attr_setschedpolicy(&tattr, SCHED_RR);

	/* With new priority specified */
	pthread_create(&thread, NULL, SV_Select_Thread, NULL);

	if(gopt.verbose)
		printf("SV_Select thread started\n");
}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void SV_Select::Stop()
{
	pthread_cancel(thread);
	pthread_join(thread, NULL);

	if(gopt.verbose)
		printf("SV_Select thread stopped\n");
}
/*----------------------------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------------------------*/
SV_Select::SV_Select()
{

	sv = 0;
	mode = WARM_START;
	mask_angle = PI/2;

	pnav = &input_s.master_nav;
	pclock = &input_s.master_clock;

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
SV_Select::~SV_Select()
{

	printf("Destructing SV_Select\n");

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void SV_Select::Inport()
{

	int32 bread;

	/* Pend on PVT sltn */
	bread = sizeof(PVT_2_SV_Select_S);
	while(bread == sizeof(PVT_2_SV_Select_S))
		bread = read(PVT_2_SV_Select_P[READ], &input_s, sizeof(PVT_2_SV_Select_S));

	/* If the PVT is less than 1 minutes old, still use it */
	if((pnav->stale_ticks < (60*TICS_PER_SECOND)) && pnav->initial_convergence)
	{
		mode = HOT_START;
		MaskAngle();
	} /* Warm start, only use for visibility, give it a 5 minute limit though */
	else if(pnav->stale_ticks < (360*TICS_PER_SECOND))
	{
		mode = WARM_START;
		MaskAngle();
	} /* Cold start, hardest way of doing things */
	else
	{
		mode = COLD_START;
	}

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void SV_Select::Acquire()
{
	int32 lcv, chan, already;

	chan = 666;
	already = 0;

	/* See if any correlators are available */
	pthread_mutex_lock(&mInterrupt);

		/* If an empty channel exists, ask for an acquisition */
		for(lcv = 0; lcv < MAX_CHANNELS; lcv++)
			if(gInterrupt[lcv] == 0)
			{
				chan = lcv;
				break;
			}

		sv_prediction[sv].tracked = false;

		/* If the SV is already being tracked skip the acquisition */
		for(lcv = 0; lcv < MAX_CHANNELS; lcv++)
			if(pChannels[lcv]->getActive())
				if(pChannels[lcv]->getSV() == sv)
				{
					already = 666;
					sv_prediction[sv].tracked = true;
				}

	pthread_mutex_unlock(&mInterrupt);

	/* Update prediction if the SV is being tracked */
//	if(already == 666)
//	{
//		GetAlmanac(sv);
//		SV_Position(sv);
//		SV_LatLong(sv);
//		SV_Predict(sv);
//		UpdateState();
//		return;
//	}

	/* Run the SV prediction routine based on Almanac data */
	GetAlmanac(sv);
	SV_Position(sv);
	SV_LatLong(sv);
	SV_Predict(sv);

	/* Do something with acquisition */
	if(chan != 666)
	{
		/* Tell the acquisition what to try */
		if(SetupRequest())
		{

			/* Send to the acquisition thread */
			write(Trak_2_Acq_P[WRITE], &request, sizeof(Acq_Request_S));

			/* Wait for acq to return, do stuff depending on the state */
			read(Acq_2_Trak_P[READ], &result, sizeof(Acq_Result_S));

			/* Pass over channel */
			result.chan = chan;

			/* Do something! */
			if(already != 666)
				ProcessResult();

		}

		UpdateState();

	}

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void SV_Select::Export()
{

	output_s.mask_angle = mask_angle;
	output_s.mode = mode;
	output_s.type = sv_history[sv].type;

	memcpy(&output_s.sv_predicted[0], &sv_prediction[0], NUM_CODES*sizeof(Acq_Predicted_S));
	memcpy(&output_s.sv_history[0], &sv_history[0], NUM_CODES*sizeof(Acq_History_S));
	write(SV_Select_2_Telem_P[WRITE], &output_s, sizeof(SV_Select_2_Telem_S));

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
bool SV_Select::SetupRequest()
{

	Acq_Predicted_S *ppred;
	int32 doppler;

	/* Initialize parameters */
	request.state = 1;
	request.type = sv_history[sv].type;
	request.sv = sv;
	request.mindopp = -MAX_DOPPLER;
	request.maxdopp = MAX_DOPPLER;
	sv_history[sv].mindopp = -MAX_DOPPLER;
	sv_history[sv].maxdopp = MAX_DOPPLER;

	if(almanacs[sv].decoded == false)
		return(true);

	if(mode == COLD_START)
	{
		return(true);
	}
	else if(mode == WARM_START) /* On warm start on use predicted visibility, not Doppler information */
	{
		ppred = &sv_prediction[sv];

		if(ppred->visible)
			return(true);
		else
			return(false);
	}
	else /* Else use Doppler information to narrow Doppler search space */
	{

		ppred = &sv_prediction[sv];

		if(ppred->visible)
		{

			/* Round to a kHz value */
			doppler = (int32)ppred->doppler;
			doppler = doppler - (doppler % 1000);

			/* Give it a 3 kHz error range */
			request.mindopp = (doppler - 1000);
			request.maxdopp = (doppler + 1000);
			sv_history[sv].mindopp = (doppler - 1000);
			sv_history[sv].maxdopp = (doppler + 1000);

			return(true);

		}
		else
		{
			return(false);
		}
	}

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void SV_Select::ProcessResult()
{

	int32 type;
	Acq_History_S *psv;

	psv = &sv_history[sv];
	type = psv->type;

	if((mode == HOT_START) && (almanacs[sv].decoded))
	{
		/* Always a better estimate from the PVT and Alamanac than the acq engine */
		result.doppler = sv_prediction[sv].doppler;
	}

	if(result.success)
	{

		psv->count[type]++;
		psv->attempts[type]++;
		psv->successes[type]++;
		psv->magnitude = result.magnitude;
		psv->doppler = result.doppler;

		/* Map receiver channels to channels on correlator */
		write(Trak_2_Corr_P[result.chan][WRITE], &result, sizeof(Acq_Result_S));

	}
	else
	{

		psv->count[type]++;
		psv->failures[type]++;
		psv->attempts[type]++;
		psv->magnitude = result.magnitude;
		psv->doppler = result.doppler;

		if(psv->count[type] >= ACQ_ITERATIONS)
		{
			psv->count[type] = 0;
			psv->type++;

			if(psv->type > ACQ_MAX)
				psv->type = ACQ_STRONG;
		}

	}

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void SV_Select::UpdateState()
{

	sv++;
	if(sv >= NUM_CODES)
		sv = 0;

	//sv = 0;
}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void SV_Select::GetAlmanac(int32 _sv)
{

	pEphemeris->Lock();
	almanacs[_sv] = pEphemeris->getAlmanac(_sv);
	pEphemeris->Unlock();

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void SV_Select::SV_Position(int32 _sv)
{

	int32 iter;
	double a, n0, M, E, P, R, I, L;
	double sE, cE, sI, cI, sL, cL, sP, cP;
	double dEdM, ecc, sqrt1mee, dtemp, Xp, Yp;
	double Edot, Pdot, Rdot, Idot, Xpdot, Ypdot, Ldot, sPdot, cPdot;
	double tk;

	Almanac_S *alm;
	SV_Position_S *psv;

	if(almanacs[_sv].decoded)
	{

		alm = &almanacs[_sv];
		psv = &sv_positions[_sv];

		/* Time to calculate position */
		tk = pclock->time - alm->toa;

		if (tk > HALF_OF_SECONDS_IN_WEEK)
			tk -= SECONDS_IN_WEEK;
		else if (tk < (-HALF_OF_SECONDS_IN_WEEK))
			tk += SECONDS_IN_WEEK;

		/* Mean motion */
		a = alm->sqrta * alm->sqrta;
		n0 = sqrt(GRAVITY_CONSTANT/(a*a*a));

		/* Mean anomaly, M (rads). */
		M = alm->m0 + n0 * tk;
		M = fmod(M, TWO_PI);

		/* Obtain eccentric anomaly E by solving Kepler's equation. */
		ecc = alm->ecc;

		sqrt1mee = sqrt (1.0 - ecc * ecc);
		E = M;
		for (iter = 0; iter < 20; iter++)
		{
			sE = sin(E);
			cE = cos(E);
			dEdM = 1.0 / (1.0 - ecc * cE);
			if (fabs (dtemp = (M - E + ecc * sE) * dEdM) < 1.0E-14)
				break;
			E += dtemp;
		}

		Edot = dEdM * n0;

			/* Compute the argument of latitude, P. */
		P = atan2 (sqrt1mee * sE, cE - ecc) + alm->argp;
		Pdot = sqrt1mee * dEdM * Edot;
		sP = sin(P);
		cP = cos(P);
		sPdot = cP*Pdot;
		cPdot = -sP*Pdot;

		/* Compute the corrected radius, R. */
		R = a * (1.0 - ecc * cE);
		Rdot = a * ecc * sE * Edot;

		/* Compute the corrected orbital inclination, I. */
		I = alm->in0; Idot = 0.0;
		sI = sin (I); cI = cos (I);

		/* Compute the satellite's position in its orbital plane, (Xp,Yp) */
		Xp = R * cP;
		Yp = R * sP;

		/* Compute the longitude of the ascending node, L. */
		L = alm->om0 + tk * (alm->omd - (double)WGS84OE) - (double)WGS84OE * alm->toa;
		Ldot = alm->omd - (double)WGS84OE;
		sL = sin (L); cL = cos (L);

		/* Compute the satellite's position in space, (x,y,z). */
		psv->x = Xp * cL - Yp * cI * sL;
		psv->y = Xp * sL + Yp * cI * cL;
        psv->z = Yp * sI;

		/* Compute the satellite's velocity in its orbital plane, (Xpdot,Ypdot) */
		Xpdot = Rdot * cP + R * cPdot;
		Ypdot = Rdot * sP + R * sPdot;

		/* Satellite's velocity, (vx,vy,vz). */
		psv->vx = -Ldot * (psv->y)
		+ Xpdot * cL
		- Ypdot * cI * sL;

		psv->vy = Ldot * (psv->x)
		+ Xpdot * sL
		+ Ypdot * cI * cL;

		psv->vz = Ypdot * sI;

         /* Compute SV clock correction */
		psv->time = tk;
		psv->clock_bias = alm->af0 + tk * alm->af1;
		psv->frequency_bias = alm->af1;

	}

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void SV_Select::SV_Predict(int32 _sv)
{

	double elev;
	double azim;
	double dx, dy, dz;
	double ct, cp;
	double st, sp;
	double sct, scp;
	double sst, ssp;
	double theta;
	double phi;
	double rho;
	double e, n, u;
	double radius;
	double dt;
	double relvel;
	double a, b;

	SV_Position_S *psv;
	Acq_Predicted_S *ppred;

	if(almanacs[_sv].decoded)
	{

		psv = &sv_positions[_sv];
		ppred = &sv_prediction[_sv];

		/* Use the SV position and PVT sltn to make a prediction of: Elev, Azim, Delay, Doppler */
		theta = pnav->longitude; phi = pnav->latitude;
		ct = cos(theta); st = sin(theta);
		cp = cos(phi);   sp = sin(phi);

		dx = (psv->x - pnav->x);
		dy = (psv->y - pnav->y);
		dz = (psv->z - pnav->z);

		radius = sqrt(dx*dx + dy*dy + dz*dz);

		dt = radius / SPEED_OF_LIGHT;

		a = sqrt(pnav->x*pnav->x + pnav->y*pnav->y + pnav->z*pnav->z);  //Vehicle
		b = sqrt(psv->x*psv->x + psv->y*psv->y + psv->z*psv->z);		//SV

		/* Law of cosines beotch! OR Dot product relationship */
		rho   = acos((-dx*pnav->x - dy*pnav->y - dz*pnav->z)/(a*radius));//Boresight angle of SV relative to Vehicle
		theta = acos((dx*psv->x + dy*psv->y + dz*psv->z)/(b*radius));	 //Boresight angle of vehicle relative to SV

		/* Predict visibility */
		if((rho > mask_angle) && (theta < (PI/4)))
			ppred->visible = true;
		else
			ppred->visible = false;

		dx /= radius;
		dy /= radius;
		dz /= radius;

		/* Elevation of SV relative to Vehicle (cone of exclusion) */
		e = -st*dx    +  ct*dy;
		n = -sp*ct*dx + -sp*st*dy + cp*dz;
		u =  cp*ct*dx +  cp*st*dy + sp*dz;

		rho = sqrt(n*n + e*e);

		ppred->sv = _sv;
		ppred->elev = atan2(u, rho);
		ppred->azim = atan2(e, n);

		/* Velocity along line of sight */
		relvel	 = 	dx * (pnav->vx - psv->vx) +
			      	dy * (pnav->vy - psv->vy) +
			      	dz * (pnav->vz - psv->vz);

		/* Predict time of flight in terms of chips */
		ppred->delay = dt + pnav->clock_bias/SPEED_OF_LIGHT - psv->clock_bias; 	/* Seconds */

		/* Predict observed doppler in terms of Hz */
		ppred->doppler = relvel - pnav->clock_rate - psv->frequency_bias*SPEED_OF_LIGHT;/* meters/second */
		ppred->doppler = ppred->doppler*L1/SPEED_OF_LIGHT;								/* Hz */


	}

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void SV_Select::SV_LatLong(int32 _sv)
{

	/* This is bullshitted as a spherical earth! */
	float a = 6378137;
	float b = 6356752.314;
	float eprime2 = 0.00673949681994;
	float e2 = 0.00669438006676;
	float p;
	float theta;
	float N;
	float latitude;
	float longitude;
	float altitude;

	SV_Position_S *psv;

	if(almanacs[_sv].decoded)
	{

		psv			= &sv_positions[_sv];

		p			= sqrt(psv->x*psv->x + psv->y*psv->y);

		theta		= atan( (psv->z*a)/ (p*b) );

		latitude	= atan( (psv->z+eprime2*b*sin(theta)*sin(theta)*sin(theta)) /
						(p-e2*a*cos(theta)*cos(theta)*cos(theta))  );

		longitude	= atan2(psv->y/p, psv->x/p);

		N			= a*a / sqrt(a*a*cos(latitude)*cos(latitude) + b*b*sin(latitude)*sin(latitude));

		altitude	= p/cos(latitude) - N;

		psv->latitude	= latitude;
		psv->longitude	= longitude;
		psv->altitude	= altitude;
	}

}
/*----------------------------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------------------------*/
void SV_Select::MaskAngle()
{

	float radius;
	float a = 6378137;
	float b = 6356752.314;
	float c;

	c = .5*(a + b);

	if(pnav->altitude < 1000)
	{
		mask_angle = PI/2;
	}
	else
	{

		radius = pnav->altitude + c;
		mask_angle = (PI/2) - acos(c/radius); //Boresight, not an elevation!!!!
	}

	/* Add in additional mask */
	mask_angle += MASK_ANGLE*DEG_2_RAD;

}
/*----------------------------------------------------------------------------------------------*/



