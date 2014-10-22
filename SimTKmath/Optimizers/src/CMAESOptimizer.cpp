/* -------------------------------------------------------------------------- *
 *                        Simbody(tm): SimTKmath                              *
 * -------------------------------------------------------------------------- *
 * This is part of the SimTK biosimulation toolkit originating from           *
 * Simbios, the NIH National Center for Physics-Based Simulation of           *
 * Biological Structures at Stanford, funded under the NIH Roadmap for        *
 * Medical Research, grant U54 GM072970. See https://simtk.org/home/simbody.  *
 *                                                                            *
 * Portions copyright (c) 2006-14 Stanford University and the Authors.        *
 *                                                                            *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may    *
 * not use this file except in compliance with the License. You may obtain a  *
 * copy of the License at http://www.apache.org/licenses/LICENSE-2.0.         *
 *                                                                            *
 * Unless required by applicable law or agreed to in writing, software        *
 * distributed under the License is distributed on an "AS IS" BASIS,          *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   *
 * See the License for the specific language governing permissions and        *
 * limitations under the License.                                             *
 * -------------------------------------------------------------------------- */

#include "CMAESOptimizer.h"

#include <bitset>

namespace SimTK {

#define SimTK_CMAES_PRINT(cmds) \
do { \
if (std::bitset<2>(diagnosticsLevel).test(0)) { cmds; } \
} \
while(false)

#define SimTK_CMAES_FILE(cmds) \
do { \
if (std::bitset<2>(diagnosticsLevel).test(1)) { cmds; } \
} \
while(false)

CMAESOptimizer::CMAESOptimizer(const OptimizerSystem& sys) : OptimizerRep(sys)
{
    SimTK_VALUECHECK_ALWAYS(2, sys.getNumParameters(), INT_MAX, "nParameters",
            "CMAESOptimizer");
}

Optimizer::OptimizerRep* CMAESOptimizer::clone() const {
    return( new CMAESOptimizer(*this) );
}

Real CMAESOptimizer::optimize(SimTK::Vector& results)
{ 
    // Initialize objective function value and cmaes data structure.
    // =============================================================
    Real f; 
    cmaes_t evo;

    const OptimizerSystem& sys = getOptimizerSystem();
    int n = sys.getNumParameters();

    // Check that the initial point is feasible.
    // =========================================
    checkInitialPointIsFeasible(results);
    
    // Initialize cmaes.
    // =================
    double* funvals = init(evo, results);

	
    // Resume a previous simulation?
    // =============================
    // TODO clean up.
	bool isresume = false; 
    getAdvancedBoolOption("resume", isresume );

    // TODO where does this go?
	if (isresume) {
		cmaes_resume_distribution(&evo, (char*)"resumecmaes.dat"); 
	}


    SimTK_CMAES_PRINT(printf("%s\n", cmaes_SayHello(&evo)));

    // TODO cmaes_ReadSignals(&evo, "cmaes_signals.par");

    // TODO let the master also run an objective function evaluation.
	
    // Optimize.
    // =========
	while (!cmaes_TestForTermination(&evo)) {

        // Sample a population.
        // ====================
		double*const* pop = cmaes_SamplePopulation(&evo);

        // Resample to keep population within limits.
        // ==========================================
    	if( sys.getHasLimits() ) {
    		Real *lowerLimits, *upperLimits;
        	sys.getParameterLimits( &lowerLimits, &upperLimits );
			
			for (int i = 0; i < cmaes_Get(&evo, "lambda"); i++) {
				bool feasible = false; 
				while (!feasible) {
					feasible = true; 
					for (int j = 0; j < n; j++) {
						if (pop[i][j] < lowerLimits[j] || 
								pop[i][j] > upperLimits[j]) {
							feasible = false; 
							pop = cmaes_ReSampleSingle(&evo, i); 
							break; 
						}
					}
				}
			}
		}

        // Evaluate the objective function on the samples.
        // ===============================================
        for (int i = 0; i < cmaes_Get(&evo, "lambda"); i++) {
            objectiveFuncWrapper(n, pop[i], true, &funvals[i], this);
        }
		
        // Update the distribution (mean, covariance, etc.).
        // =================================================
		cmaes_UpdateDistribution(&evo, funvals);
		
        // TODO only if users specify a signals files.
        // TODO cmaes_ReadSignals(&evo, "cmaes_signals.par");
	}

    // Wrap up.
    // ========
    SimTK_CMAES_PRINT(printf("Stop:\n%s\n", cmaes_TestForTermination(&evo)));

    // Update best-yet parameters and objective function value.
    const double* optx = cmaes_GetPtr(&evo, "xbestever");
    for (int i = 0; i < n; i++) {
        results[i] = optx[i]; 
    }
    f = cmaes_Get(&evo, "fbestever");

	SimTK_CMAES_FILE(cmaes_WriteToFile(&evo, "resume", "resumecmaes.dat"));
	SimTK_CMAES_FILE(cmaes_WriteToFile(&evo, "all", "allcmaes.dat"));

    // Free memory.
    cmaes_exit(&evo);
	
	return f;  
}

void CMAESOptimizer::checkInitialPointIsFeasible(const Vector& x) const {

    const OptimizerSystem& sys = getOptimizerSystem();

    if( sys.getHasLimits() ) {
        Real *lowerLimits, *upperLimits;
        sys.getParameterLimits( &lowerLimits, &upperLimits );
        for (int i = 0; i < sys.getNumParameters(); i++) {
            SimTK_APIARGCHECK4_ALWAYS(
                    lowerLimits[i] <= x[i] && x[i] <= upperLimits[i],
                    "CMAESOptimizer", "checkInitialPointIsFeasible",
                    "Initial guess results[%d] = %f "
                    "is not within limits [%f, %f].",
                    i, x[i], lowerLimits[i], upperLimits[i]);
        }
    }
}

double* CMAESOptimizer::init(cmaes_t& evo, SimTK::Vector& results) const
{
    const OptimizerSystem& sys = getOptimizerSystem();
    int n = sys.getNumParameters();

    // Prepare to call cmaes_init.
    // ===========================

    // lambda
    // ------
	int numsamples = 0;
    getAdvancedIntOption("lambda", numsamples );
	if (numsamples == 0) {
		numsamples = 4+int(3*std::log((double)n)); 
        // TODO unnecessary.
	}
	
    // sigma
    // -----
    // TODO move to processBefore, and change to 0.3
	double stepsize = 0;
    getAdvancedRealOption("sigma", stepsize ); 
	if (stepsize == 0.0) {
		stepsize = 0.1; 
	}
	Vector stepsizeArray(n);
	for (int i = 0; i < n; i++) {
		stepsizeArray[i] = stepsize;  
	}

    // seed
    // ----
    int seed = 0;
    if (getAdvancedIntOption("seed", seed)) {
        SimTK_VALUECHECK_NONNEG_ALWAYS(seed, "seed",
                "CMAESOptimizer::processSettingsBeforeCMAESInit");
    }

    // Call cmaes_init_para.
    // =====================
	// Here, we specify the subset of options that can be passed to
    // cmaes_init_para.
	cmaes_init_para(&evo,
            n,                 // dimension
            &results[0],       // xstart
            &stepsizeArray[0], // stddev
            seed,              // seed
            numsamples,        // lambda
            "writeonly"              // input_parameter_filename TODO change depending on advanced parameters.
            // TODO "non"              // input_parameter_filename
            ); 

    // Settings that are usually read in from cmaes_initials.par.
    // ==========================================================
    processSettingsAfterCMAESInit(evo);

    // Once we've updated settings in readpara_t, finalize the initialization.
    return cmaes_init_final(&evo);
}

void CMAESOptimizer::processSettingsAfterCMAESInit(cmaes_t& evo) const
{
    // stopMaxIter
    // ===========
    // maxIterations is a protected member variable of OptimizerRep.
	evo.sp.stopMaxIter = maxIterations;

    // stopTolFun
    // ==========
    // convergenceTolerance is a protected member variable of OptimizerRep.
    evo.sp.stopTolFun = convergenceTolerance;

    // stopMaxFunEvals
    // ===============
    int stopMaxFunEvals;
    if (getAdvancedIntOption("stopMaxFunEvals", stopMaxFunEvals)) {
        SimTK_VALUECHECK_NONNEG_ALWAYS(stopMaxFunEvals, "stopMaxFunEvals",
                "CMAESOptimizer::processSettingsAfterCMAESInit");
        evo.sp.stopMaxFunEvals = stopMaxFunEvals;
    }

    // maxtime
    // =======
    double maxtime;
    if (getAdvancedRealOption("maxTimeFractionForEigendecomposition", maxtime))
    {
        evo.sp.updateCmode.maxtime = maxtime;
    }
}

#undef SimTK_CMAES_PRINT

} // namespace SimTK
