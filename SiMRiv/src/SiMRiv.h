#include <R.h>
#include <Rdefines.h>
#include <Rinternals.h>
#include <Rmath.h>
//#include "rand32.h"
#define SCALARCHAR(x)		Rf_ScalarString(mkChar(x))
#define MULTIPLIER			1000000		// multiplier for bounded values (probabilites)
#define ANGLERES			31			// PDFs are discrete here. this is the angular resolution used for discrete circular PDFs. MUST be odd.
#define ANGLECENTER			15			// MUST be = (ANGLERES-1)/2
#define ANGLESTEP			0.2026834f	// MUST be = 2*PI/(ANGLERES)
#define ACCUMULATORRESOLUTION	20		// resolution of the raster accumulator function. Each step is divided into this many microsteps.
#define TOLERANCECIRCNORM	0.00001		// circular normal is the sum of an infinite series. this is the tolerance to say when to stop summing.

typedef enum {false=0, true=1} bool;
typedef unsigned int BOUNDED;		// this is to accomodate all values that are bounded between 0 and 1. Let's assume that MULTIPLIER corresponds to 1, to keep integer math
typedef unsigned long CDF[ANGLERES];		// cumulative density function (discrete)
typedef float PDF[ANGLERES];
typedef enum {CIRCULAR,GAUSSIAN} PERCEPTIONWINDOWTYPE;
typedef struct {
	double x,y;		// this has to be double. if float, not enough precision for very small steps.
} POINT;

typedef struct {
	float radius;			// radius or sigma, depending on the type NOTE: if gaussian, the computations should be done at least until 4*sigma
	PERCEPTIONWINDOWTYPE type;
	float *weights;
} PERCEPTIONWINDOW;

typedef struct {
	double taconc,steplength;		// turning angle concentration
	PERCEPTIONWINDOW pwind;
	PDF basePDF;			// the original angularPDF with integral 1
	PDF scaledPDF;			// basePDF/max(basePDF) - to use as a multiplier
	CDF cumPDF;
} STATE;

typedef struct {
	SEXP pspecies;
	POINT curpos;
	float curang;
	unsigned short curstate,nstates;
	double *transitionmatrix;
	STATE *states;
} INDIVIDUAL;

typedef struct {
	unsigned int ncols,nrows;
	long xmin,ymin,xmax,ymax,width,height,ncells;
	float xscale,yscale;
	const double *values;
	SEXP pvalues;
} RASTER;

inline float circNormalTerm(float ang,float var,int k) {
	return((1/sqrt(var * 2 * PI)) * exp(-( (ang + 2 * PI * (float)k)*(ang + 2 * PI * (float)k) )/(2 * var)));
}

SEXP _simulate_individuals(SEXP _individuals, SEXP _starting_positions, SEXP _timespan, SEXP _angles, SEXP _resist, SEXP envir, SEXP _parallel, SEXP _nrepetitions);
SEXP stepRasterAccumulator(SEXP relocs,SEXP _resist,SEXP envir);
void circNormal(float rho,float* out,float* scaledout);
float drawRandomAngle(unsigned long *pdf);
SEXP getRasterExtent(SEXP raster,SEXP rho);
SEXP getRasterDim(SEXP raster,SEXP rho);
SEXP getRasterValues(SEXP raster,SEXP rho);
SEXP getListElement(SEXP list, const char *str);
SEXP getRasterRes(SEXP raster,SEXP rho);
RASTER *openRaster(SEXP raster,SEXP rho);
void closeRaster(RASTER *raster);
double extractRasterValue(const RASTER *raster,float x,float y);
double extractRasterValueNoNaN(const RASTER *raster,float x,float y);
void computeEmpiricalResistancePDF(POINT curpos, const RASTER *resist,PERCEPTIONWINDOW *percwind,PDF pdf);
float computeLengthMove(double baseStepLength,POINT curpos,RASTER *resist,float angle);
void rotatePDF(PDF pdf,PDF out,float ang);
int densityRand(int nValues, unsigned long *cdf);

// Kernel density

typedef struct {
	unsigned int nrecords;
	float max,sum;
	unsigned char *density;
} DENSITY;

float *buildKernel(int side,float *sigma,int nDimensions,int *outkernelhalfside,int *outkernelside,int *outkernelsidesq);

