#include <omp.h>
#include "SiMRiv.h"
SEXP rho;

SEXP _simulate_individuals(SEXP _individuals, SEXP _starting_positions, SEXP _timespan, SEXP _angles, SEXP _resist, SEXP envir, SEXP _parallel, SEXP _nrepetitions) {
	rho=envir;
	RASTER *resist = NULL;
	GetRNGstate();	// FIXME: RNG is not thread safe! We cannot parallelize like this! We must generate random numbers before parallel simulations.
	
	if(_resist != R_NilValue) {
		resist = openRaster(_resist,rho);
//		rasterRes=NUMERIC_POINTER(getRasterRes(_resist,rho))[0];	// TODO handle cases when there is more than one raster (minimum resolution!)
	}
	
	int timespan = INTEGER_POINTER(_timespan)[0], *start = INTEGER_POINTER(_starting_positions);
	int nrepetitions = INTEGER_POINTER(_nrepetitions)[0];
	double *angles = NULL;
	double *prelocs;
	unsigned int ninds=LENGTH(_individuals),i,j,k;
	SEXP relocs,tmp3,tmp4;
	const char *tmp5;
	float curangtrans;
	bool parallel = LOGICAL_POINTER(_parallel)[0];
	
	if(_angles != R_NilValue)
		angles = NUMERIC_POINTER(_angles);
	
	if(parallel && ninds > 1 && nrepetitions > 1) Rf_error("If more than one individual is supplied, number of repetitions must be 1.");

// pointers to individual data
	INDIVIDUAL *ind=malloc(sizeof(INDIVIDUAL)*ninds);
	
	PROTECT(relocs=allocMatrix(REALSXP, timespan, 3 * (nrepetitions > 1 ? nrepetitions : ninds)));	// output is a matrix with columns x,y,state appended for each individual
	prelocs=NUMERIC_POINTER(relocs);
	
// get pointers to individual, species and state data
	for(i=0;i<ninds;i++) {
		//tmp=VECTOR_ELT(_individuals,i);
		//ind[i].pspecies=GET_SLOT(tmp,SCALARCHAR("species"));
		ind[i].pspecies=VECTOR_ELT(_individuals,i);
		ind[i].transitionmatrix=NUMERIC_POINTER(GET_SLOT(ind[i].pspecies,SCALARCHAR("transitionMatrix")));
		ind[i].nstates=LENGTH(GET_SLOT(ind[i].pspecies,SCALARCHAR("states")));
//		printf("NStates %d\n",ind[i].nstates);
		ind[i].states=malloc(sizeof(STATE)*ind[i].nstates);		
		for(j=0;j<ind[i].nstates;j++) {
			tmp3=VECTOR_ELT(GET_SLOT(ind[i].pspecies,SCALARCHAR("states")),j);
			ind[i].states[j].taconc=NUMERIC_POINTER(GET_SLOT(tmp3,SCALARCHAR("turningAngleConcentration")))[0];
			ind[i].states[j].steplength=NUMERIC_POINTER(GET_SLOT(tmp3,SCALARCHAR("stepLength")))[0];
			//ind[i].states[j].stubb=NUMERIC_POINTER(GET_SLOT(tmp3,SCALARCHAR("stubb")))[0];
			tmp4=GET_SLOT(tmp3,SCALARCHAR("perceptualRange"));
			ind[i].states[j].pwind.radius=NUMERIC_POINTER(GET_SLOT(tmp4,SCALARCHAR("parameters")))[0];
			tmp5=CHAR(STRING_ELT(GET_SLOT(tmp4,SCALARCHAR("type")),0));
			if(strcmp(tmp5,"circular")==0)
				ind[i].states[j].pwind.type=CIRCULAR;
			else if(strcmp(tmp5,"gaussian")==0)
				ind[i].states[j].pwind.type=GAUSSIAN;
			else error("Invalid perception window type.");
			
			// let's create a lookup table for perception window weights!
			if(ind[i].states[j].pwind.type==GAUSSIAN) {
				float pwsigma=ind[i].states[j].pwind.radius;
				ind[i].states[j].pwind.radius=(int)ceil(ind[i].states[j].pwind.radius*4);	// NOTE: radius is taken as sigma and increased 4 times
				ind[i].states[j].pwind.weights=calloc(ind[i].states[j].pwind.radius+1,sizeof(float));
				for(k=0;k<ind[i].states[j].pwind.radius;k++) {
					ind[i].states[j].pwind.weights[k]=exp(-(double)k*k/(2*pwsigma*pwsigma));
					//Rprintf("%f ",ind[i].states[j].pwind.weights[k]);
				}
			}

//			if(ind[i].states[j].steplength>0 && minimumRes>ind[i].states[j].steplength) minimumRes=ind[i].states[j].steplength;
		}
		
//for(j=0;j<ind[i].nstates*ind[i].nstates;j++) {Rprintf("%d ",ind[i].transitionmatrix[j]);}
//Rprintf("sp: %s\n",STRING_VALUE(GET_SLOT(ind[i].pspecies,SCALARCHAR("name"))));
	}

// START SIMULATION
	{
		unsigned int tmp1, tmp2, time;
		PDF tmpPDF, tmprotPDF;
		CDF tmpMultCDF;
		unsigned long r, s, k;
		float lengthmove;
		STATE *tmpstate;
// assign initial states, angles and starting positions
		for(i=0; i<ninds; i++) {
			ind[i].curpos.x = start[i];
			ind[i].curpos.y = start[i + ninds];
			ind[i].curstate = runif(0, ind[i].nstates - 1);	// random initial state
			ind[i].curang = angles ? (ISNAN(angles[i]) ? drawRandomAngle(NULL) : ((angles[i] + PI) / ANGLESTEP)) : drawRandomAngle(NULL);		// uniform random angle
// initialize states
			for(j=0; j<ind[i].nstates; j++) {
// compute base circular PDFs for all states of all individuals (centered on 0), with given concentration
				circNormal(ind[i].states[j].taconc, ind[i].states[j].basePDF, ind[i].states[j].scaledPDF);
// compute the respective cumulative circular PDFs
				for(k=1, ind[i].states[j].cumPDF[0] = (long)(ind[i].states[j].basePDF[0]*MULTIPLIER); k<ANGLERES; k++) {
					ind[i].states[j].cumPDF[k] = ind[i].states[j].cumPDF[k-1]+(long)(ind[i].states[j].basePDF[k]*MULTIPLIER);
				}
			}
		}
/***************************************
** MAIN TIME LOOP
***************************************/
// NOTE: time is unitless for now

		if(parallel) {
			int curind, curtime, curstate, curindaddr;
			float curang;
			POINT curpos;
			double curtrans[100];
			int tmsize;
			const long indcolsize = timespan * 3;
			double *colX, *colY, *colS;
			int niterations = nrepetitions > 1 ? nrepetitions : ninds;
			
			#pragma omp parallel for private(curindaddr, colX, colY, colS, r, k, s, tmp1, tmp2, time, tmpstate, tmprotPDF, tmpPDF, j, tmpMultCDF, curangtrans, lengthmove, curind, curtime, curstate, curang, curpos, curtrans, tmsize) num_threads(8)
			for(curind=0; curind<niterations; curind++) {
				colX = &prelocs[curind * indcolsize];
				colY = &prelocs[curind * indcolsize + timespan];
				colS = &prelocs[curind * indcolsize + timespan * 2];
				curindaddr = nrepetitions > 1 ? 0 : curind;
/*#pragma omp critical
Rprintf("%d-%d ", curind, ninds);*/
				tmsize = ind[curindaddr].nstates * ind[curindaddr].nstates;
				for(int z=0; z<tmsize; z++)
					curtrans[z] = ind[curindaddr].transitionmatrix[z];	// for now, constant transition matrix
				curang = ind[curindaddr].curang;
				curpos.x = ind[curindaddr].curpos.x;
				curpos.y = ind[curindaddr].curpos.y;
				curstate = ind[curindaddr].curstate;
				for(time=0; time<timespan; time++) {
// draw new state according to transition matrix
					r = runif(0, MULTIPLIER-1);
					for(k=0, s=(unsigned long)(curtrans[curstate] * MULTIPLIER)
						; k < (ind[curindaddr].nstates - 1) && r >= s
						; k++, s += (unsigned long)(curtrans[curstate + k * ind[curindaddr].nstates] * MULTIPLIER)) {}
					curstate = k; //>= ind[i].nstates ? (ind[i].nstates - 1) : k;		// is this condition needed?

					tmpstate = &ind[curindaddr].states[curstate];

// rotate the 0-centered base PDF (the one calculated from the state's concentration parameter)
// to center on the previous step angle
					rotatePDF(tmpstate->scaledPDF, tmprotPDF, curang - ANGLECENTER);

// compute the empirical resistance PDF around the current position
					computeEmpiricalResistancePDF(curpos, resist, &tmpstate->pwind, tmpPDF);

					if(tmpPDF[0] != -1) {		// resistance is heterogeneous
// multiply rotated base PDF by empirical PDF and compute cumulative PDF directly
						for(j=1, tmpMultCDF[0] = (unsigned long)(tmprotPDF[0] * tmpPDF[0] * MULTIPLIER); j<ANGLERES; j++)
							tmpMultCDF[j] = tmpMultCDF[j-1] + (long)(tmprotPDF[j] * tmpPDF[j] * MULTIPLIER);

// TODO what to do when the desired direction is facing towards an infinite resistance area and there is no overlap of PDFs? keep trying, or abort step?
						if(tmpMultCDF[ANGLERES-1] == 0)
							curang = drawRandomAngle(NULL);	// here we just draw a uniform random angle
						else
// draw random angle based on compound PDF (resistance + correlated components)
							curang = drawRandomAngle(tmpMultCDF);

					} else {	// ignore resistance when resistance is equal in all directions
						for(j=1, tmpMultCDF[0]=(unsigned long)(tmprotPDF[0]*MULTIPLIER); j<ANGLERES; j++)
							tmpMultCDF[j] = tmpMultCDF[j-1] + (unsigned long)(tmprotPDF[j]*MULTIPLIER);
// draw random angle based on base PDF
						curang = drawRandomAngle(tmpMultCDF);
					}

					curangtrans = curang * ANGLESTEP - PI;
	//				Rprintf("%.3f %.3f %.3f | ", tmpstate->steplength, ind[i].curpos.x, ind[i].curpos.y);
					lengthmove = computeLengthMove(tmpstate->steplength, curpos, resist, curangtrans);
					if(lengthmove > 0) {
						curpos.x += cos(curangtrans) * lengthmove;
						curpos.y += sin(curangtrans) * lengthmove;
					}
					
					if(isnan(curpos.x)) {	// this should never happen, but...
						Rprintf("%f %f %f %f %f %f\n", tmpstate->steplength, curpos.x, curpos.y, lengthmove, curang, curangtrans);
						Rf_error("Unexpected error, please report.");
					}

					*colX = curpos.x;
					*colY = curpos.y;
					*colS = curstate;
					
					colX++;
					colY++;
					colS++;
/*
					tmp1 = curind * timespan * 3;	// this is the column offset of the current individual in the output matrix

					prelocs[tmp1 + curtime] = curpos.x;
					prelocs[tmp1 + timespan + curtime] = curpos.y;
					prelocs[tmp1 + timespan + timespan + curtime] = curstate;
					*/
				}
			}
			
		} else {
			double **curtrans = malloc(sizeof(double*) * ninds);
			for(time=0; time<timespan; time++) {
				for(i=0; i<ninds; i++)
					curtrans[i] = ind[i].transitionmatrix;	// for now, constant transition matrix

	// LOOP FOR EACH INDIVIDUAL
	// TODO: we can parallelize this if we just want a batch of non-interacting simulations
				for(i=0, tmp2=0; i<ninds; i++, tmp2+=timespan) {	// tmp2 is just a relative pointer to output matrix
	// draw new state according to transition matrix
					r = runif(0, MULTIPLIER-1);
					for(k=0, s=(unsigned long)(curtrans[i][ind[i].curstate] * MULTIPLIER)
						; k < (ind[i].nstates - 1) && r >= s
						; k++, s += (unsigned long)(curtrans[i][ind[i].curstate + k * ind[i].nstates] * MULTIPLIER)) {}
					ind[i].curstate = k; //>= ind[i].nstates ? (ind[i].nstates - 1) : k;		// is this condition needed?
				
					tmpstate = &ind[i].states[ind[i].curstate];
	// rotate the 0-centered base PDF (the one calculated from the state's concentration parameter)
	// to center on the previous step angle
					rotatePDF(tmpstate->scaledPDF, tmprotPDF, ind[i].curang-ANGLECENTER);
	// compute the empirical resistance PDF around the current position
					computeEmpiricalResistancePDF(ind[i].curpos, resist, &tmpstate->pwind, tmpPDF);
					if(tmpPDF[0]!=-1) {		// resistance is heterogeneous
	// multiply rotated base PDF by empirical PDF and compute cumulative PDF directly
						for(j=1, tmpMultCDF[0] = (unsigned long)(tmprotPDF[0] * tmpPDF[0] * MULTIPLIER); j<ANGLERES; j++)
							tmpMultCDF[j] = tmpMultCDF[j-1] + (long)(tmprotPDF[j] * tmpPDF[j] * MULTIPLIER);

	// TODO what to do when the desired direction is facing towards an infinite resistance area and there is no overlap of PDFs? keep trying, or abort step?
						if(tmpMultCDF[ANGLERES-1] == 0)
							ind[i].curang = drawRandomAngle(NULL);	// here we just draw a uniform random angle
						else
	// draw random angle based on compound PDF (resistance + correlated components)
							ind[i].curang = drawRandomAngle(tmpMultCDF);

					} else {	// ignore resistance when resistance is equal in all directions
						for(j=1, tmpMultCDF[0]=(unsigned long)(tmprotPDF[0]*MULTIPLIER); j<ANGLERES; j++)
							tmpMultCDF[j] = tmpMultCDF[j-1] + (unsigned long)(tmprotPDF[j]*MULTIPLIER);
	// draw random angle based on base PDF
						ind[i].curang = drawRandomAngle(tmpMultCDF);
					}
	/*for(j=0;j<ANGLERES;j+=1) Rprintf("%.01f ",tmpstate->scaledPDF[j]);
	Rprintf("\n");
	for(j=0;j<ANGLERES;j+=1) Rprintf("%.01f ",tmprotPDF[j]);
	Rprintf("\n");*/

					curangtrans = ind[i].curang * ANGLESTEP - PI;
	//				Rprintf("%.3f %.3f %.3f | ", tmpstate->steplength, ind[i].curpos.x, ind[i].curpos.y);
					lengthmove = computeLengthMove(tmpstate->steplength, ind[i].curpos, resist, curangtrans);
					if(lengthmove > 0) {
						ind[i].curpos.x += cos(curangtrans) * lengthmove;
						ind[i].curpos.y += sin(curangtrans) * lengthmove;
					}
					if(isnan(ind[i].curpos.x)) {	// this should never happen, but...
						Rprintf("%f %f %f %f %f %f\n", tmpstate->steplength, ind[i].curpos.x, ind[i].curpos.y, lengthmove, ind[i].curang, curangtrans);
						Rf_error("Unexpected error, please report.");
					}

					tmp1=tmp2*3;	// this is the column offset of the current individual in the output matrix
					prelocs[tmp1 + time]=ind[i].curpos.x;
					prelocs[tmp1 + timespan + time]=ind[i].curpos.y;
					prelocs[tmp1 + timespan + timespan + time]=ind[i].curstate;
				}
			}	// time loop
			free(curtrans);
		}	// if(parallel)
	}
	
	PutRNGstate();
	
	UNPROTECT(1);
	for(i=0;i<ninds;i++) {
		for(j=0;j<ind[i].nstates;j++) {
			if(ind[i].states[j].pwind.type==GAUSSIAN) free(ind[i].states[j].pwind.weights);
		}
		free(ind[i].states);
	}
	free(ind);
	if(_resist!=R_NilValue) closeRaster(resist);
	return relocs;
}

/*
	foreach individual {	// loop for updating states, transition matrices, etc.
		CURTRANSMATRIX = transitionMatrix(allpositions,resist,...)
	}
	
	foreach individual {	// loop for moving each individual one step (pixel by pixel)
		CURSTATE = draw state according to CURTRANSMATRIX
		STEPLENGTH = get the step length of this individual at CURSTATE (or draw a step length according to the distribution)
		TURNANG = draw a turning angle from distribution in CURSTATE (which is relative to previous absolute angle, i.e., how much it deviates from previous angle)
		
		for(counter=0;counter<STEPLENGTH;counter++) {	// loop for moving pixel by pixel during one step
			if(CURPOS (current position), rounded to nearest pixel, is different from previous) {
				PDF = computePDF(CURPOS,TURNANG,perceptionwindow[CURSTATE],stubbornness[CURSTATE],resistance,...)
			}
			ANG = draw random angle from PDF
			LENGTHMOVE = computeLengthmove(resistance,ANG,...)
			displace CURPOS LENGTHMOVE pixels in ANG direction
		}
		push CURPOS into TRAJ
	}
*/

/*
make a circular wrapped normal PDF centered on zero
note that there's an offset of -PI, so the first value corresponds to the density of -PI
If scaledout is provided, also output range-standardized PDF
Code adapted from the CircStats package, function dwrpnorm()
_rho is the concentration parameter, varying from 0 (flat) to 1 (one peak)
*/
void circNormal(float _rho,float* out,float* scaledout) {
	float var=-2.f*log(_rho),next,last,delta;
	float ang,max=-1;
	int k,i;
    
	for(i=0,ang=-PI;i<=ANGLECENTER;i++,ang+=ANGLESTEP) {
		k=0;
		next=circNormalTerm(ang,var,k);
    	delta=1;
		while(delta>TOLERANCECIRCNORM) {
		    k++;
		    last=next;
		    next=last+circNormalTerm(ang,var,k) + circNormalTerm(ang,var,-k);
		    delta=fabs(next-last);
		}
		if(next>max) max=next;
		out[i]=next;
	}
	for(i=ANGLECENTER;i<ANGLERES;i++) out[i]=out[(ANGLERES-1)-i];
	
	if(scaledout) {		// range-standardize
		for(i=0;i<ANGLERES;i++) scaledout[i]=out[i]/max;
	}
}

// draws a random angle from CDF or a uniform random angle if pdf=NULL
float drawRandomAngle(CDF cdf) {
//return 0;
//return (ANGLERES-1);
	if(cdf==NULL) {
		return runif(0, ANGLERES-1);
	} else {
		if(cdf[ANGLERES-1]==0) return runif(0, ANGLERES-1);
		return densityRand(ANGLERES, cdf);
/*		int i;
		int r = unif_rand() * cdf[ANGLERES-1];
		//int r=rand32() % cdf[ANGLERES-1];	// FIXME this may be biased for large cdf values
		for(i=0;i<ANGLERES && r>cdf[i];i++) {}
		return i;*/
	}
}

int densityRand(int nValues, unsigned long *cdf) {
	long lo, hi, mid;
	float r;
	lo = 0;
	hi = nValues - 1;
	r = cdf[hi] * unif_rand();

	while(lo < hi) {
		mid = (lo + hi) / 2;
		if (r < cdf[mid]) hi = mid; else lo = mid + 1;
	}
	return lo;
}

/**
* Computes the actual distance moved in a given direction, taking into account the resistance
* that has to be crossed between the two desired points.
*/
inline float computeLengthMove(double baseStepLength,POINT curpos,RASTER *resist,float angle) {
	if(resist == NULL) return baseStepLength;
	// here we take a simple approach: just compute the mean resistance between the starting point and the (theoretical) end point
	float tmp1=(float)extractRasterValueNoNaN(resist,curpos.x+cos(angle) * baseStepLength,curpos.y+sin(angle) * baseStepLength);
	if(tmp1>0.999) return 0;		// if it is infinite resistence, do not go there!
	float tmp=(float)extractRasterValueNoNaN(resist,curpos.x,curpos.y);
	return baseStepLength*(1-(tmp+tmp1)/2);
}

/*
* Computes the circular EPDF at a given location, according to resistance values in the surroundings
* and angular bias
* Returns the PDF in the provided pointer NOTE: the PDF has an arbitrary scale, the integral is not constant!!
*/
void computeEmpiricalResistancePDF(POINT curpos,const RASTER *resist,PERCEPTIONWINDOW *percwind,PDF pdf) {
	bool allinf=true;
	float step;
	
	if(resist == NULL) {
		pdf[0]=-1;
		return;
	}
	
	switch(percwind->type) {
	case CIRCULAR:
		step=percwind->radius/ACCUMULATORRESOLUTION;
		int i,j;
		float tcos,tsin,ang,sum;
		POINT tmppos;
		double tmp;
		
		for(i=0; i<ANGLERES; i++) {	// make a whole circle
			ang=-PI+i*ANGLESTEP;
			tmppos=curpos;
	// for each angle, sum the resistance values along the radial line centered on curpos
			tcos=cos(ang)*step;	// TODO create a look-up table to speed up things here? or maybe not?
			tsin=sin(ang)*step;

			for(j=0, sum=0; j<percwind->radius; j += step) {
				tmp=extractRasterValue(resist,tmppos.x,tmppos.y);

				if(isnan(tmp) || tmp==NA_REAL || tmp==1)
					sum+=0;
				else {
					sum+=1-tmp;
					if(allinf) allinf=false;
				}
				tmppos.x+=tcos;
				tmppos.y+=tsin;
			}
			pdf[i] = sum;
//			cdf[i]=(unsigned long)(sum*MULTIPLIER);
//				ang+=ANGLESTEP;
		}
		break;
		
	case GAUSSIAN:
		step=percwind->radius/ACCUMULATORRESOLUTION;	// TODO is this adequate in the gaussian?
		{
			int i,j;
			float tcos,tsin,ang,sum;
			POINT tmppos;
			double tmp;
			
			for(i=0;i<ANGLERES;i++) {	// make a whole circle
				ang=-PI+i*ANGLESTEP;
				tmppos=curpos;
		// for each angle, sum the resistance values along the radial line centered on curpos
				tcos=cos(ang)*step;	// TODO create a look-up table to speed up things here? or maybe not?
				tsin=sin(ang)*step;

				for(j=0,sum=0;j<percwind->radius;j+=step) {
					tmp=extractRasterValue(resist,tmppos.x,tmppos.y);

					if(isnan(tmp) || tmp==NA_REAL || tmp==1)
						sum+=0;
					else {
						sum+=(1-tmp) * percwind->weights[(int)floor(j)];
						allinf=false;
					}
//					Rprintf("hori %f step %f %d %f\n",horizon,step,j,percwind->weights[(int)floor(j)]);
					tmppos.x+=tcos;
					tmppos.y+=tsin;
				}
				
				pdf[i]=sum;
			}
		}
		break;
		
	default:
		Rprintf("PW: %d\n", (int)percwind->type);
		error("Perception window type not implemented yet");
		break;
	}
	if(allinf) pdf[0]=-1;
}

/**
	Shifts the PDF by offset
*/
void rotatePDF(PDF pdf,PDF out,float ang) {return;
//	int offset=((int)(ang*(ANGLERES-1)/(2*PI))) % (ANGLERES-1),i,j;
//	int offset=(int)lroundf(ang*ANGLERES/(2*PI)+0.5) +1 ,i,j;	//% ANGLERES
	int offset=ANGLERES-ang,j,i;
//  (24*0.2513274-pi)*25/(2*pi)+0.5
//	int offset=((int)lroundf(ang*ANGLERES/(2*PI))) % ANGLERES,i,j;
	for(i=0,j=offset;i<ANGLERES;i++,j++) out[i]=pdf[ (j<0 ? ANGLERES+j : j % ANGLERES)];
}

/**
	Computes the mean value of the given raster "infinitesimally" along each step of the movement. I.e., integrates the raster along each segment of the path and divides by its length
	relocs is assumed to be a matrix whose first 2 columns are X and Y
	"infinitesinally" means that it discretizes each step into ACCUMULATORRESOLUTION pieces, so the microstep length is variable.
*/
SEXP stepRasterAccumulator(SEXP relocs,SEXP _resist,SEXP envir) {
	if(_resist==R_NilValue) error("Must provide a raster");
	rho=envir;
	RASTER *resist;
	SEXP out;
	double *prelocs=NUMERIC_POINTER(relocs),tmp,*pout;
	//float microstep=getEmpiricalCDFStep();
	int nrelocs=(int)INTEGER_POINTER(GET_DIM(relocs))[0]-1;		// -1 cause the loop excludes the last one
	double vx,vy;
	float sum;
	POINT tmppos,endpoint;
	int i,j,nsteps;
	
	resist=openRaster(_resist,rho);
	PROTECT(out=NEW_NUMERIC(nrelocs));	// output is a vector with the accumulated resistance in each step
	pout=NUMERIC_POINTER(out);
// loop of the steps
	for(i=0;i<nrelocs;i++) {
		tmppos.x=prelocs[i];
		tmppos.y=prelocs[i+nrelocs+1];
		endpoint.x=prelocs[i+1];
		endpoint.y=prelocs[i+1+nrelocs+1];
// compute a vector with length of microstep, with the direction of the step
		vx=endpoint.x-tmppos.x;
		vy=endpoint.y-tmppos.y;
/*		hyp=sqrt(vx*vx + vy*vy);
		vx=(vx*microstep)/hyp;
		vy=(vy*microstep)/hyp;
		nsteps=(int)(hyp/microstep);*/
		vx/=ACCUMULATORRESOLUTION;
		vy/=ACCUMULATORRESOLUTION;
		nsteps=ACCUMULATORRESOLUTION;
		
//Rprintf("nrel %d nmicro %d vec %f %f\n",nrelocs,nsteps,vx,vy);
// "integrate" raster values between starting and ending points of this step
		for(j=0,sum=0; j<nsteps; j++,tmppos.x+=vx,tmppos.y+=vy) {
			tmp=extractRasterValue(resist,tmppos.x,tmppos.y);	// TODO: optimize this, we don't need to call a function with calculations here, just integrate and avoid calculations
			if(isnan(tmp) || tmp==NA_REAL || tmp==1)
				sum+=1;
			else
				sum+=tmp;
		}
		pout[i]=sum/nsteps;
		/*
		if(hyp>0)
			pout[i]=sum/hyp;
		else
			pout[i]=0;
			*/
	}
	
	closeRaster(resist);
	UNPROTECT(1);
	return out;
}


