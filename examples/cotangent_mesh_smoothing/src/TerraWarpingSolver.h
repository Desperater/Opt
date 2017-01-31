#pragma once

#include <cassert>

#include "cutil.h"
#include "../../shared/CudaArray.h"
#include "../../shared/Precision.h"
extern "C" {
#include "Opt.h"
}

template <class type> type* createDeviceBuffer(const std::vector<type>& v) {
	type* d_ptr;
	cutilSafeCall(cudaMalloc(&d_ptr, sizeof(type)*v.size()));

	cutilSafeCall(cudaMemcpy(d_ptr, v.data(), sizeof(type)*v.size(), cudaMemcpyHostToDevice));
	return d_ptr;
}

class TerraWarpingSolver {

	int* d_headX;
	int* d_headY;
	
	int* d_tailX;
	int* d_tailY;

	int* d_prevX;
	int* d_nextX;

	int edgeCount;

public:
	TerraWarpingSolver(unsigned int vertexCount, unsigned int E, int* d_xCoords, int* d_offsets, const std::string& terraFile, const std::string& optName) : m_optimizerState(nullptr), m_problem(nullptr), m_plan(nullptr)
	{
		edgeCount = (int)E;
		m_optimizerState = Opt_NewState();
		m_problem = Opt_ProblemDefine(m_optimizerState, terraFile.c_str(), optName.c_str());

		std::vector<int> yCoords;

		for (int y = 0; y < (int)edgeCount; ++y) {
			yCoords.push_back(0);
		}

		d_headY = createDeviceBuffer(yCoords);
		d_tailY = createDeviceBuffer(yCoords);

		int* h_offsets = (int*)malloc(sizeof(int)*(vertexCount + 1));
		cutilSafeCall(cudaMemcpy(h_offsets, d_offsets, sizeof(int)*(vertexCount + 1), cudaMemcpyDeviceToHost));

		int* h_xCoords = (int*)malloc(sizeof(int)*(edgeCount * 3));
		cutilSafeCall(cudaMemcpy(h_xCoords, d_xCoords, sizeof(int)*(edgeCount * 3), cudaMemcpyDeviceToHost));
		h_xCoords[edgeCount] = vertexCount;

        m_unknownCount = vertexCount;

		// Convert to our edge format
		std::vector<int> h_headX;
		std::vector<int> h_tailX;
		std::vector<int> h_prevX;
		std::vector<int> h_nextX;
		for (int headX = 0; headX < (int)vertexCount; ++headX) {
			for (int j = h_offsets[headX]; j < h_offsets[headX + 1]; ++j) {
				h_headX.push_back(headX);
				h_tailX.push_back(h_xCoords[3 * j + 0]);
				h_prevX.push_back(h_xCoords[3 * j + 1]);
				h_nextX.push_back(h_xCoords[3 * j + 2]);
				//std::cout << h_headX.back() << " " << h_tailX.back() << std::endl;
			}
		}

		d_headX = createDeviceBuffer(h_headX);
		d_tailX = createDeviceBuffer(h_tailX);
		d_prevX = createDeviceBuffer(h_prevX);
		d_nextX = createDeviceBuffer(h_nextX);

		uint32_t dims[] = { vertexCount, 1 };
		m_plan = Opt_ProblemPlan(m_optimizerState, m_problem, dims);

		assert(m_optimizerState);
		assert(m_problem);
		assert(m_plan);

		free(h_offsets);
		free(h_xCoords);
	}

	~TerraWarpingSolver()
	{
		cutilSafeCall(cudaFree(d_headX));
		cutilSafeCall(cudaFree(d_headY));
		cutilSafeCall(cudaFree(d_tailX));
		cutilSafeCall(cudaFree(d_tailY));
		cutilSafeCall(cudaFree(d_prevX));
		cutilSafeCall(cudaFree(d_nextX));

		if (m_plan) {
			Opt_PlanFree(m_optimizerState, m_plan);
		}

		if (m_problem) {
			Opt_ProblemDelete(m_optimizerState, m_problem);
		}

	}

	void solve(float3* d_unknown, float3* d_target, unsigned int nNonLinearIterations, unsigned int nLinearIterations, unsigned int nBlockIterations, float weightFit, float weightReg, std::vector<SolverIteration>& iters)
	{

		void* solverParams[] = {  &nNonLinearIterations, &nLinearIterations, &nBlockIterations };

		float weightFitSqrt = sqrt(weightFit);
		float weightRegSqrt = sqrt(weightReg);
		int * d_zero = d_headY;


        std::vector<void*> problemParams;
        CudaArray<double> d_unknownDouble, d_targetDouble;
        if (OPT_DOUBLE_PRECISION) {
            auto getDoubleArrayFromFloatDevicePointer = [](CudaArray<double>& doubleArray, float* d_ptr, int size) {
                std::vector<float> v;
                v.resize(size);
                cutilSafeCall(cudaMemcpy(v.data(), d_ptr, size*sizeof(float), cudaMemcpyDeviceToHost));
                std::vector<double> vDouble;
                vDouble.resize(size);
                for (int i = 0; i < size; ++i) {
                    vDouble[i] = (double)v[i];
                }
                doubleArray.update(vDouble);
            };


            getDoubleArrayFromFloatDevicePointer(d_unknownDouble, (float*)d_unknown, m_unknownCount * 3);
            getDoubleArrayFromFloatDevicePointer(d_targetDouble, (float*)d_target, m_unknownCount * 3);


            problemParams = { &weightFitSqrt, &weightRegSqrt, d_unknownDouble.data(), d_targetDouble.data(), &edgeCount, d_headX, d_zero, d_tailX, d_zero, d_prevX, d_zero, d_nextX, d_zero };
        }
        else {
            problemParams = { &weightFitSqrt, &weightRegSqrt, d_unknown, d_target, &edgeCount, d_headX, d_zero, d_tailX, d_zero, d_prevX, d_zero, d_nextX, d_zero };
        }

		launchProfiledSolve(m_optimizerState, m_plan, problemParams.data(), solverParams, iters);
        m_finalCost = Opt_ProblemCurrentCost(m_optimizerState, m_plan);

        if (OPT_DOUBLE_PRECISION) {
            size_t size = m_unknownCount * 3;
            std::vector<double> vDouble(size);
            std::vector<float> v(size);

            cutilSafeCall(cudaMemcpy(vDouble.data(), d_unknownDouble.data(), size*sizeof(double), cudaMemcpyDeviceToHost));
            for (int i = 0; i < size; ++i) {
                v[i] = (float)vDouble[i];
            }
            cutilSafeCall(cudaMemcpy(d_unknown, v.data(), size*sizeof(float), cudaMemcpyHostToDevice));
        }
	}

    double finalCost() const {
        return m_finalCost;
    }

private:
    int m_unknownCount;
    Opt_State*	    m_optimizerState;
	Opt_Problem*    m_problem;
    Opt_Plan*		m_plan;
    double m_finalCost = nan(nullptr);
};