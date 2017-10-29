
#include <mpi.h>
#include <exception>
#include <iostream>
#include <cmath>
#include <cstring>
#include "shared.h"

const int N_INVALID = -1;

enum Neighbour {
	LEFT = 0,
	TOP = 1,
	RIGHT = 2,
	BOTTOM = 3,
};

class ClusterManager {
public:
	ClusterManager() {
		MPI_Init(nullptr, nullptr);
		MPI_Comm_rank(comm, &nodeId);
		MPI_Comm_size(comm, &nodeCount);

		auto sqr = static_cast<long>(std::sqrt(nodeCount));
		if(sqr*sqr != nodeCount) {
			if(nodeId == 0) {
				err_log() << "Number of nodes must be power of some integer (got " << nodeCount << " )" << std::endl;
			}

			throw std::runtime_error("incorrect node count!");
		} else {
			sideLen = sqr;
		}

		row = nodeId/sideLen;
		column = nodeId%sideLen;

		initNeighbours();

		err_log() << "Cluster initialized successfully. I'm (" << row << "," << column << ")" << std::endl;
	}

	int getNodeCount() {
		return nodeCount;
	}

	int getNodeId() {
		return nodeId;
	}

	std::ostream& err_log() {
		std::cerr << "[" << nodeId << "] ";
		return std::cerr;
	}

	int* getNeighbours() {
		return &neighbours[0];
	}


	MPI_Comm getComm() {
		return comm;
	}

	~ClusterManager() {
		MPI_Finalize();
	}

private:
	const static auto comm = MPI_COMM_WORLD;

	int nodeCount;
	int sideLen;
	int nodeId;

	int row;
	int column;
	int neighbours[4];

	void initNeighbours() {
		if(row == 0) { neighbours[Neighbour::TOP] = N_INVALID; }
		else { neighbours[Neighbour::TOP] = nodeId-sideLen; }

		if(row == sideLen-1) { neighbours[Neighbour::BOTTOM] = N_INVALID; }
		else { neighbours[Neighbour::BOTTOM] = nodeId+sideLen; }

		if(column == 0) { neighbours[Neighbour::LEFT] = N_INVALID; }
		else { neighbours[Neighbour::LEFT] = nodeId-1; }

		if(column == sideLen-1) { neighbours[Neighbour::RIGHT] = N_INVALID; }
		else { neighbours[Neighbour::RIGHT] = nodeId+1; }

		err_log() << "Neighbours: "
	          << " LEFT: " << neighbours[LEFT]
	          << " TOP: " << neighbours[TOP]
	          << " RIGHT: " << neighbours[RIGHT]
	          << " BOTTOM: " << neighbours[BOTTOM] << std::endl;
	}
};

class Comms {
public:
	Comms(const Coord innerLength) : innerLength(innerLength) {
		reset();
	}

	void exchange(int targetId, NumType* sendBuffer, NumType* receiveBuffer) {
		MPI_Isend(sendBuffer, innerLength, NUM_MPI_DT, targetId, 1, MPI_COMM_WORLD, rq + nextId);
		MPI_Irecv(receiveBuffer, innerLength, NUM_MPI_DT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, rq + nextId + 1);

		nextId += 2;
	}

	void wait() {
		int toFinish = nextId;
		while(toFinish > 0) {
			int finished = 0;
			MPI_Waitsome(RQ_COUNT, rq, &finished, idxArray, MPI_STATUSES_IGNORE);
			toFinish -= finished;
		}
	}

	void reset() {
		for(int i = 0; i < RQ_COUNT; i++) {
			rq[i] = MPI_REQUEST_NULL;
		}
		nextId = 0;
	}

private:
	const static int RQ_COUNT = 8;
	const Coord innerLength;
	MPI_Request rq[RQ_COUNT];
	int idxArray[RQ_COUNT];
	int nextId;
};

/**
 * Work area is indexed from 1 (first element) to size (last element)
 */
class Workspace {
public:
	Workspace(const Coord size, const NumType borderCond, ClusterManager& cm, Comms& comm)
			: innerLength(size), actualSize(size*size), cm(cm), borderCond(borderCond), comm(comm)
	{
		neigh = cm.getNeighbours();
		allocateBuffers();
	}

	~Workspace() {
		freeBuffers();
	}

	void set_elf(const Coord x, const Coord y, const NumType value) {
		// copying to send buffers occurs during comms phase
		front[x*innerLength+y] = value;
	}

	NumType get_elb(const Coord x, const Coord y) {
		if(x == -1) {
			if(y == -1) {
				// conrner - invalid query, we never ask about it
				throw std::runtime_error("corner access!");
			} else if (y == innerLength+1) {
				// corner - invalid query
				throw std::runtime_error("corner access!");
			} else {
				// left outer border
				if(neigh[LEFT] != N_INVALID) {
					return outerEdge[LEFT][y];
				} else {
					return borderCond;
				}
			}	
		} else if (x == innerLength+1) {
			if(y == -1) {
				// conrner - invalid query, we never ask about it
				throw std::runtime_error("corner access!");
			} else if (y == innerLength+1) {
				// corner - invalid query
				throw std::runtime_error("corner access!");
			} else {
				// right outer border
				if(neigh[RIGHT] != N_INVALID) {
					return outerEdge[RIGHT][y];
				} else {
					return borderCond;
				}
			}
		} else {
			if(y == -1) {
				// top edge
				if(neigh[BOTTOM] != N_INVALID) {
					return outerEdge[BOTTOM][x];
				} else {
					return borderCond;
				}
			} else if (y == innerLength+1) {
				// bottom edge
				if(neigh[TOP] != N_INVALID) {
					return outerEdge[TOP][x];
				} else {
					return borderCond;
				}
			} else {
				// coords within main area
				return back[x*innerLength+y];
			}
		}
	}

	Coord getLength() {return innerLength;}

	void swap() {
		// @todo: comms here!
		copyInnerEdgesToBuffers();

		comm.reset();
		for(int i = 0; i < 4; i++) {
			auto iThNeigh = neigh[i];
			if(iThNeigh != N_INVALID) {
				comm.exchange(iThNeigh, innerEdge[i], outerEdge[i]);
			}
		}
		comm.wait();

		swapBuffers();
	}

private:
	ClusterManager& cm;
	Comms& comm;
	int* neigh;

	const Coord innerLength;
	const Coord actualSize;

	const NumType borderCond;

	/* horizontal could be stored with main buffer, but for convenience both horizontals and
	 * verticals are allocated separatelly (and writes mirrored) */
	NumType* innerEdge[4];
	/* all outer edges are allocated separatelly; their length is innerLength, not innerLength + 2 */
	NumType* outerEdge[4];
	NumType *front;
	NumType *back;

	void allocateBuffers() {
		front = new NumType[actualSize];
		back = new NumType[actualSize];

		for(int i = 0; i < 4; i++) {
			if(neigh[i] != N_INVALID) {
				innerEdge[i] = new NumType[innerLength];
				outerEdge[i] = new NumType[innerLength];
			} else {
				innerEdge[i] = nullptr;
				outerEdge[i] = nullptr;
			}
		}

		// flipping buffers requires to flip poitners to inner/outer ones (if part is shared -> don't share?)
		// outer buffers are only associated with back buffer, but inner are mainly associated with both
	}

	void freeBuffers() {
		delete[] front;
		delete[] back;

		for(int i = 0; i < 4; i++) {
			if(innerEdge != nullptr) {
				delete[] innerEdge[i];
				delete[] outerEdge[i];
			}
		}
	}

	NumType* elAddress(const Coord x, const Coord y, NumType* base) {
		return base + innerLength*x + y;
	}

	void swapBuffers() {
		NumType* tmp = front;
		front = back;
		back = tmp;
	}

	void copyInnerEdgesToBuffers() {
		#define LOOP(EDGE, X, Y, BUFF) \
		if(neigh[EDGE] != N_INVALID) { \
			for(Coord i = 0; i < innerLength; i++) { \
				innerEdge[EDGE][i] = *elAddress(X,Y,BUFF); \
			} \
		}

		LOOP(TOP, i, innerLength-1, front)
		LOOP(BOTTOM, i, 0, front)
		LOOP(LEFT, 0, i, front)
		LOOP(RIGHT, innerLength-1, i, front)

		#undef LOOP
	}
};


int main(int argc, char **argv) {
	auto conf = parse_cli(argc, argv);

	ClusterManager clusterManager;
	Comms comm(conf.N);
	Workspace w(conf.N, 0.0, clusterManager, comm);

	std::cout << "parallel algorithm" << std::endl;

	return 0;
}