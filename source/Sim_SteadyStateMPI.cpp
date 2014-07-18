/* *
 * Sim_SteadyStateMPI.cpp
 *
 * Created by Fabian Wermelinger on 7/18/14.
 * Copyright 2014 ETH Zurich. All rights reserved.
 * */
#include <cassert>
#include <omp.h>
#include <string>
#include <fstream>
#include <iomanip>

#include "Sim_SteadyStateMPI.h"
#include "HDF5Dumper_MPI.h"

using namespace std;


Sim_SteadyStateMPI::Sim_SteadyStateMPI(const int argc, const char ** argv, const int isroot_)
    : isroot(isroot_), t(0.0), step(0), fcount(0), mygrid(NULL), myGPU(NULL), parser(argc, argv)
{
    _setup();
    assert(mygrid != NULL);

    _allocGPU();
    assert(myGPU != NULL);
}


void Sim_SteadyStateMPI::_setup()
{
    // parse mandatory arguments
    parser.set_strict_mode();
    tend         = parser("-tend").asDouble();
    CFL          = parser("-cfl").asDouble();
    nslices      = parser("-nslices").asInt();
    dumpinterval = parser("-dumpinterval").asDouble();
    saveinterval = parser("-saveinterval").asInt();
    parser.unset_strict_mode();

    // parse optional aruments
    verbosity = parser("-verb").asInt(0);
    restart   = parser("-restart").asBool(false);
    nsteps    = parser("-nsteps").asInt(0);

    // MPI
    npex = parser("-npex").asInt(1);
    npey = parser("-npey").asInt(1);
    npez = parser("-npez").asInt(1);

    // assign dependent stuff
    tnextdump = dumpinterval;
    mygrid    = new GridMPI(npex, npey, npez);

    // setup initial condition
    if (restart)
    {
        if(_restart())
        {
            printf("Restarting at step %d, physical time %f\n", step, t);
            _dump("restart_ic");
            tnextdump = (fcount+1)*dumpinterval;
        }
        else
        {
            printf("Loading restart file was not successful... Abort\n");
            exit(1);
        }
    }
    else
    {
        _ic();
        _dump();
    }
}


void Sim_SteadyStateMPI::_allocGPU()
{
    myGPU = new GPUlabSteadyState(*mygrid, nslices, verbosity);
}


void Sim_SteadyStateMPI::_ic()
{
    // default initial condition
    const double r  = parser("-rho").asDouble(1.0);
    const double u  = parser("-u").asDouble(0.0);
    const double v  = parser("-v").asDouble(0.0);
    const double w  = parser("-w").asDouble(0.0);
    const double p  = parser("-p").asDouble(1.0);
    const double g  = parser("-g").asDouble(1.4);
    const double pc = parser("-pc").asDouble(0.0);

    const double ru = r*u;
    const double rv = r*v;
    const double rw = r*w;
    const double G  = 1.0/(g - 1.0);
    const double P  = g*pc*G;
    const double e  = G*p + P + 0.5*r*(u*u + v*v + w*w);

    typedef GridMPI::PRIM var;
    GridMPI& icgrid = *mygrid;

#pragma omp paralell for
    for (int iz = 0; iz < GridMPI::sizeZ; ++iz)
        for (int iy = 0; iy < GridMPI::sizeY; ++iy)
            for (int ix = 0; ix < GridMPI::sizeX; ++ix)
            {
                icgrid(ix, iy, iz, var::R) = r;
                icgrid(ix, iy, iz, var::U) = ru;
                icgrid(ix, iy, iz, var::V) = rv;
                icgrid(ix, iy, iz, var::W) = rw;
                icgrid(ix, iy, iz, var::E) = e;
                icgrid(ix, iy, iz, var::G) = G;
                icgrid(ix, iy, iz, var::P) = P;
            }
}


void Sim_SteadyStateMPI::_dump(const string basename)
{
    sprintf(fname, "%s_%04d", basename.c_str(), fcount++);
    printf("Dumping file %s at step %d, time %f...\n", fname, step, t);
    DumpHDF5_MPI<GridMPI, myTensorialStreamer>(*mygrid, step, fname);
}


void Sim_SteadyStateMPI::_save()
{
    const string dump_path = parser("-fpath").asString(".");

    if (isroot)
    {
        ofstream saveinfo("save.info");
        saveinfo << setprecision(15) << t << endl;
        saveinfo << step << endl;
        saveinfo << fcount << endl;
        saveinfo.close();
    }
    DumpHDF5_MPI<GridMPI, mySaveStreamer>(*mygrid, step, "save.data", dump_path);
}


bool Sim_SteadyStateMPI::_restart()
{
    const string dump_path = parser("-fpath").asString(".");

    ifstream saveinfo("save.info");
    if (saveinfo.good())
    {
        saveinfo >> t;
        saveinfo >> step;
        saveinfo >> fcount;
        ReadHDF5_MPI<GridMPI, mySaveStreamer>(*mygrid, "save.data", dump_path);
        return true;
    }
    else
        return false;
}


void Sim_SteadyStateMPI::run()
{
    const double h = mygrid->getH();
    float sos;
    double dt;

    while (t < tend)
    {
        // 1.) Compute max SOS -> dt
        const double tsos = _maxSOS<MaxSpeedOfSound_CUDA>(&myGPU, sos);
        /* const double tsos = _maxSOS<MaxSpeedOfSound_CPP>(mygrid.pdata(), sos); */
        assert(sos > 0);
        if (verbosity) printf("sos = %f (took %f sec)\n", sos, tsos);

        dt = cfl*h/sos;
        dt = (tend-t) < dt ? (tend-t) : dt;
        dt = (tnext-t) < dt ? (tnext-t) : dt; // accurate time for file dump

        MPI_Allreduce(MPI_IN_PLACE, &dt, 1, MPI_DOUBLE, MPI_MIN, mygrid->getCartComm());

        // 2.) Compute RHS and update using LSRK3
        double trk1, trk2, trk3;
        {// stage 1
            myGPU.load_ghosts();
            trk1 = _LSRKstep<Lab>(0, 1./4, dt/h, myGPU);
            if (verbosity) printf("RK stage 1 takes %f sec\n", trk1);
        }
        {// stage 2
            myGPU.load_ghosts();
            trk2 = _LSRKstep<Lab>(-17./32, 8./9, dt/h, myGPU);
            if (verbosity) printf("RK stage 2 takes %f sec\n", trk2);
        }
        {// stage 3
            myGPU.load_ghosts();
            trk3 = _LSRKstep<Lab>(-32./27, 3./4, dt/h, myGPU);
            if (verbosity) printf("RK stage 3 takes %f sec\n", trk3);
        }
        if (verbosity) printf("netto step takes %f sec\n", tsos + trk1 + trk2 + trk3);

        t += dt;
        ++step;

        printf("step id is %d, physical time %f (dt = %f)\n", step, t, dt);

        if ((float)t == (float)tnext)
        {
            tnext += dump_interval;
            DumpHDF5_MPI<GridMPI, myTensorialStreamer>(mygrid, step, _make_fname(fname, "data", fcount++));
        }
        /* if (step % 10 == 0) DumpHDF5_MPI<GridMPI, myTensorialStreamer>(mygrid, step, _make_fname(fname, "data", fcount++)); */

        if (step % save_interval == 0)
        {
            printf("Saving time step...\n");
            _save<GridMPI>(mygrid, step, t, fcount, isroot);
        }

        if (step == nsteps) break;
    }

    DumpHDF5_MPI<GridMPI, myTensorialStreamer>(mygrid, step, _make_fname(fname, "data", fcount++));
}
