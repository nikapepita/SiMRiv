angle.variation <- function(relocs, nbins = 100, window.size = dim(relocs$stats)[1] %/% nbins) {
	ta <- relocs$stats[, 2]
	sda <- numeric(length(ta) %/% window.size)

	for(i in 1:length(sda)) {
		sda[i] <- sd(ta[((i - 1) * window.size + 1) : (i * window.size)])
	}
	return(sda)
}

histogram.fixed <- function(data, range, nbins, log = FALSE) {
	if(nbins == 0) return(NULL)
	if(log) {
		data = log(data + 1)
		range = log(range + 1)
	}
	bins <- seq(range[1], range[2], len = nbins + 1)
	inter <- findInterval(data, bins, rightmost.closed = TRUE)
	inter <- inter[inter > 0 & inter <= nbins]
	tr <- table(inter)
	tra <- rep(0, nbins)
#	names(tra) <- 1:nbins
	names(tra) <- paste(round(bins[1:nbins]), "-", round(bins[2:(nbins+1)]), sep="")
	tra[as.numeric(names(tr))] <- tr
	return(tra)
}

# Computes the turning angle variation histogram (variation computed in a time-moving window)
# This histogram gives a view of the relative amount of time that the animal spends in each distinct state
computeVariationHistogram <- function(relocs, nbins = 7, range = NULL, window.size = dim(relocs$stats)[1] %/% 100) {
	a.var <- angle.variation(relocs, window.size = window.size)
	if(is.null(range)) 
		range <- range(a.var)
	hist.var.ref <- histogram.fixed(a.var, range, nbins)
	return(hist.var.ref)
}

speciesModel <- function(type, perception.window = 0, steplength = 1, prob.upperbound = 1) {
	return(switch(pmatch(type, c("CRW", "RW.CRW", "CRW.CRW", "CRW.pw", "RW.CRW.sl", "CRW.CRW.sl")), {
		f <- function(parameters) {
			return(species(
				state.CRW(parameters[1])
			) * perception.window + steplength)
		}
		attr(f, "npars") <- 1
		attr(f, "lower.bounds") <- 0
		attr(f, "upper.bounds") <- 1
		attr(f, "param.names") <- "Turning angle correlation"
		return(f)
	}, {
		f <- function(parameters) {
			return(species(
				state.CRW(parameters[1]) + state.RW()
				, transitionMatrix(parameters[2], parameters[3])
			) * perception.window + steplength)
		}
		attr(f, "npars") <- 3
		attr(f, "lower.bounds") <- c(0, rep(0, 2))
		attr(f, "upper.bounds") <- c(1, rep(prob.upperbound, 2))
		attr(f, "param.names") <- c("Turning angle correlation", "Prob. CRW -> RW", "Prob. RW -> CRW")
		return(f)
	}, {
		f <- function(parameters) {
			return(species(
				state.CRW(parameters[1]) + state.CRW(parameters[2])
				, transitionMatrix(parameters[3], parameters[4])
			) * perception.window + steplength)
		}
		attr(f, "npars") <- 4
		attr(f, "lower.bounds") <- c(0, 0, rep(0, 2))
		attr(f, "upper.bounds") <- c(1, 1, rep(prob.upperbound, 2))
		attr(f, "param.names") <- c("Turning angle correlation state 1", "Turning angle correlation state 2", "Prob. st.1 -> st.2", "Prob. st.2 -> st.1")
		return(f)
	}, {
		if(perception.window <= 1) stop("You must provide the maximum allowable perception window size, e.g. perception.window = 500")
		f <- function(parameters) {
			return(species(
				state.CRW(parameters[1])
			) * parameters[2] + steplength)
		}
		attr(f, "npars") <- 2
		attr(f, "lower.bounds") <- c(0, 1)
		attr(f, "upper.bounds") <- c(1, perception.window)
		attr(f, "param.names") <- c("Turning angle correlation", "Perception window radius")
		return(f)
	}, {
		f <- function(parameters) {
			return(species(
				(state.CRW(parameters[1]) + parameters[5]) + (state.RW() + parameters[4])
				, transitionMatrix(parameters[2], parameters[3])
			) * perception.window)
		}
		attr(f, "npars") <- 5
		attr(f, "lower.bounds") <- c(0, rep(0, 4))
		attr(f, "upper.bounds") <- c(1, rep(prob.upperbound, 2), rep(steplength, 2))
		attr(f, "param.names") <- c("Turning angle correlation", "Prob. CRW -> RW", "Prob. RW -> CRW", "Step length RW", "Step length CRW")
		return(f)
	}, {
		f <- function(parameters) {
			return(species(
				(state.CRW(parameters[1]) + parameters[5]) + (state.CRW(parameters[2]) + parameters[6])
				, transitionMatrix(parameters[3], parameters[4])
			) * perception.window)
		}
		attr(f, "npars") <- 6
		attr(f, "lower.bounds") <- rep(0, 6)
		attr(f, "upper.bounds") <- c(1, 1, rep(prob.upperbound, 2), rep(steplength, 2))
		attr(f, "param.names") <- c("Turning angle correlation S1", "Turning angle correlation S2", "Prob. S1 -> S2", "Prob. S2 -> S1", "Step length S1", "Step length S2")
		return(f)
	}))
}

adjustModel <- function(
	realData
	, species.model
	, resolution = 10
# simulation parameters
	, resistance = NULL
	, coords = NULL
	, start.resistance = NULL
# fitness function parameters
	, nbins = 100
	, window.size = dim(reference$stats)[1] %/% nbins
	, nbins.hist = c(5, 5)
	, step.hist.range = c(0, 1)
	, step.hist.log = FALSE
	, nrepetitions = 1
# GA options
	, popsize = 100, generations = seq(5, 1000, by=5), mprob = 0.2
	, parallel = is.null(resistance)	# if using a raster, parallel performance doesn't increase because it is loaded in all R processes
) {
	realData <- as.matrix(realData)
	nsteps <- dim(realData)[1]
# compute turning angles and step lengths of the real movement
	reference <- sampleMovement(realData, resist = resistance)
# compute the SD of turning angles in a fixed-size juxtaposed moving window
	a.var.ref <- angle.variation(reference, window.size = window.size)
# make the (fixed-range) histogram of all the moving window SDs
# the range of the histogram is 10% larger than observed range to account for stochasticity in simulations
	increase <- (diff(range(a.var.ref)) * 0.1) / 2
	range.varta <- range(a.var.ref) + c(-increase, increase)
	hist.var.ref <- histogram.fixed(a.var.ref, range.varta, nbins.hist[1])
# make the step length histogram	
#	increase <- (diff(range(reference$stats[, "steplengths"])) * 0) / 2
#	range.step <- range(reference$stats[, "steplengths"]) + c(-increase, increase)
#	range.step[range.step < 0] = -0.001
	range.step <- quantile(reference$stats[, "steplengths"], probs = step.hist.range)
	hist.step.ref <- histogram.fixed(reference$stats[, "steplengths"], range.step, nbins.hist[2], step.hist.log)

	cl <- NULL
	cat("Real data:", nsteps,", Simulating", nsteps * resolution,"steps @ res", resolution,"\n")
	cat("Time window size for computing turning angle variation:", window.size, "\n")
	
	if(inherits(parallel, "cluster")) {
		cl <- parallel
	} else if(inherits(parallel, "logical")) {
		if(parallel) {
			cl <- makeCluster(detectCores() - 1)
		}
	} else if(inherits(parallel, "numeric")) {
		cl <- makeCluster(parallel)
	}

	if(!is.null(cl)) {	# GO PARALLEL
		clusterCall(cl, function() library(SiMRiv))
		clusterExport(cl, c("nrepetitions", "nbins.hist", "nsteps", "window.size"
			, "range.varta", "range.step", "resolution", "species.model", "resistance", "coords", "start.resistance"
			), envir = environment())
	
		objective.function.par <- function(inp.mat, reference) {
			crit <- parApply(cl, inp.mat, 1, function(inp.par, ref) {
				sp.sim <- species.model(inp.par)
	
				hist.mat <- matrix(ncol = nbins.hist[1], nrow = nrepetitions)
				hist.step <- matrix(ncol = nbins.hist[2], nrow = nrepetitions)
				if(nrepetitions == 1) {
					rel <- simulate(sp.sim, nsteps * resolution, resist = resistance, coords = coords, start.resistance = start.resistance)
					s <- sampleMovement(rel, resolution, resist = resistance)
					a.var.sim <- angle.variation(s, window.size = window.size)
					hist.var <- histogram.fixed(a.var.sim, range.varta, nbins.hist[1])
					hist.step <- histogram.fixed(s$stats[, "steplengths"], range.step, nbins.hist[2], step.hist.log)
				} else {
					for(i in 1:nrepetitions) {
						rel <- simulate(sp.sim, nsteps * resolution, resist = resistance, coords = coords, start.resistance = start.resistance)
						s <- sampleMovement(rel, resolution, resist = resistance)
						a.var.sim <- angle.variation(s, window.size = window.size)
						hist.mat[i, ] <- histogram.fixed(a.var.sim, range.varta, nbins.hist[1])
						hist.step[i, ] <- histogram.fixed(s$stats[, "steplengths"], range.step, nbins.hist[2], step.hist.log)
					}
					hist.var <- apply(hist.mat, 2, mean)
					hist.step <- apply(hist.step, 2, mean)
				}
				
				if(nbins.hist[1] > 0) {
					crit.ta <- abs(hist.var - ref[["hist.var"]])
					
					#div1 <- (hist.var[2:length(hist.var)] + 1) / (hist.var[-length(hist.var)] + 1)
					#div2 <- (ref[["hist.var"]][2:length(ref[["hist.var"]])] + 1) / (ref[["hist.var"]][-length(ref[["hist.var"]])] + 1)
					
					#div1 <- diff(hist.var)
					#div2 <- diff(ref[["hist.var"]])
					#crit.ta <- abs(div1 - div2)
				} else crit.ta <- NULL
				
				if(nbins.hist[2] > 0) {
					crit.sl <- abs(hist.step - ref[["hist.step"]])
					#div1 <- (hist.step[2:length(hist.step)] + 1) / (hist.step[-length(hist.step)] + 1)
					#div2 <- (ref[["hist.step"]][2:length(ref[["hist.step"]])] + 1) / (ref[["hist.step"]][-length(ref[["hist.step"]])] + 1)
					#div1 <- diff(hist.step)
					#div2 <- diff(ref[["hist.step"]])

					#crit.sl <- abs(div1 - div2)
				} else crit.sl <- NULL

				crit <- c(crit.ta, crit.sl)

#				crit = c(mean(abs(hist.var - ref[[3]])), sd(abs(hist.var - ref[[3]])))
				return(crit)
			}, reference)
print(crit)
			return(crit)
		}

		sol <- nsga2(objective.function.par, attr(species.model, "npars"), sum(nbins.hist) - 0, list(reference, a.var.ref, hist.var = hist.var.ref, hist.step = hist.step.ref)
			, generations = generations, popsize = popsize
			, lower.bounds = attr(species.model, "lower.bounds")
			, upper.bounds = attr(species.model, "upper.bounds")
			, vectorized = TRUE, mprob = mprob
		)
	} else {	# GO SINGLE CORE
		objective.function = function(inp.par, ref) {
			sp.sim = species.model(inp.par)
	
			hist.mat = matrix(ncol = nbins.hist[1], nrow = nrepetitions)
			hist.step <- matrix(ncol = nbins.hist[2], nrow = nrepetitions)
			if(nrepetitions == 1) {
				rel = simulate(sp.sim, nsteps * resolution, resist = resistance, coords = coords, start.resistance = start.resistance)
				s = sampleMovement(rel, resolution, resist = resistance)
				a.var.sim = angle.variation(s, window.size = window.size)
				hist.var = histogram.fixed(a.var.sim, range.varta, nbins.hist[1])
				hist.step <- histogram.fixed(s$stats[, "steplengths"], range.step, nbins.hist[2], step.hist.log)
			} else {
				for(i in 1:nrepetitions) {
					rel = simulate(sp.sim, nsteps * resolution, resist = resistance, coords = coords, start.resistance = start.resistance)
					s = sampleMovement(rel, resolution, resist = resistance)
					a.var.sim = angle.variation(s, window.size = window.size)
					hist.mat[i, ] = histogram.fixed(a.var.sim, range.varta, nbins.hist[1])
					hist.step[i, ] <- histogram.fixed(s$stats[, "steplengths"], range.step, nbins.hist[2], step.hist.log)
				}
				hist.var = apply(hist.mat, 2, mean)
				hist.step <- apply(hist.step, 2, mean)
			}
			if(nbins.hist[1] == 0) {
				crit <- abs(hist.step - ref[["hist.step"]])
			} else if(nbins.hist[2] == 0) {
				crit <- abs(hist.var - ref[["hist.var"]])
			} else {
				crit <- c(abs(hist.var - ref[["hist.var"]]), abs(hist.step - ref[["hist.step"]]))
			}
			
		#	crit = sum(abs(hist.var - ref[[3]]))
		for(o in crit)
			cat(sprintf("%5.2f ", o))
		cat("\n")
		flush.console()

			return(crit)
		}
		
		sol <- nsga2(objective.function, attr(species.model, "npars"), sum(nbins.hist), list(reference, a.var.ref, hist.var = hist.var.ref, hist.step = hist.step.ref)
			, generations = generations, popsize = popsize
			, lower.bounds = attr(species.model, "lower.bounds")
			, upper.bounds = attr(species.model, "upper.bounds")
			, vectorized = FALSE, mprob = mprob)
	}
	
	if(!is.null(cl)) stopCluster(cl)
	return(list(
		solutions = sol
		#, species = apply(sol$par, 1, species.model)
	))
}

