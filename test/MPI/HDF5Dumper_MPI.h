/*
 *  HDF5Dumper_MPI.h
 *  Cubism
 *
 *  Created by Babak Hejazialhosseini on 5/24/09.
 *  Modified by Fabian Wermelinger on 6/19/14
 *  Copyright 2009/14 CSE Lab, ETH Zurich. All rights reserved.
 *
 */

#pragma once

#include <cassert>
#include <mpi.h>
#include <iostream>

#ifdef _USE_HDF_
#include <hdf5.h>
#endif

#ifdef _FLOAT_PRECISION_
#define HDF_REAL H5T_NATIVE_FLOAT
#else
#define HDF_REAL H5T_NATIVE_DOUBLE
#endif

using namespace std;


template<typename TGrid, typename Streamer >
void DumpHDF5_MPI(TGrid &grid, const int iCounter, const string f_name, const string dump_path=".")
{
#ifdef _USE_HDF_

    int rank;
    char filename[256];
    herr_t status;
    hid_t file_id, dataset_id, fspace_id, fapl_id, mspace_id;

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    int coords[3];
    grid.peindex(coords);

    const unsigned int NX = TGrid::sizeX;
    const unsigned int NY = TGrid::sizeY;
    const unsigned int NZ = TGrid::sizeZ;
    static const unsigned int NCHANNELS = Streamer::NCHANNELS;

    if (rank==0)
      {
        cout << "Writing HDF5 file\n";
        cout << "Allocating " << (NX * NY * NZ * NCHANNELS)/(1024.*1024.*1024.) << "GB of HDF5 data\n";
      }
    Real * array_all = new Real[NX * NY * NZ * NCHANNELS];


    static const unsigned int sX = 0;
    static const unsigned int sY = 0;
    static const unsigned int sZ = 0;

    static const unsigned int eX = TGrid::sizeX;
    static const unsigned int eY = TGrid::sizeY;
    static const unsigned int eZ = TGrid::sizeZ;

    /* hsize_t count[4] = { */
    /*     TGrid::sizeX, */
    /*     TGrid::sizeY, */
    /*     TGrid::sizeZ, NCHANNELS}; */

    /* hsize_t dims[4] = { */
    /*     grid.getBlocksPerDimension(0)*TGrid::sizeX, */
    /*     grid.getBlocksPerDimension(1)*TGrid::sizeY, */
    /*     grid.getBlocksPerDimension(2)*TGrid::sizeZ, NCHANNELS}; */

    /* hsize_t offset[4] = { */
    /*     coords[0]*TGrid::sizeX, */
    /*     coords[1]*TGrid::sizeY, */
    /*     coords[2]*TGrid::sizeZ, 0}; */

    hsize_t count[4] = {
        TGrid::sizeZ,
        TGrid::sizeY,
        TGrid::sizeX, NCHANNELS};

    hsize_t dims[4] = {
        grid.getBlocksPerDimension(2)*TGrid::sizeZ,
        grid.getBlocksPerDimension(1)*TGrid::sizeY,
        grid.getBlocksPerDimension(0)*TGrid::sizeX, NCHANNELS};

    hsize_t offset[4] = {
        coords[2]*TGrid::sizeZ,
        coords[1]*TGrid::sizeY,
        coords[0]*TGrid::sizeX, 0};

    sprintf(filename, "%s/%s.h5", dump_path.c_str(), f_name.c_str());

    H5open();
    fapl_id = H5Pcreate(H5P_FILE_ACCESS);
    status = H5Pset_fapl_mpio(fapl_id, MPI_COMM_WORLD, MPI_INFO_NULL);
    file_id = H5Fcreate(filename, H5F_ACC_TRUNC, H5P_DEFAULT, fapl_id);
    status = H5Pclose(fapl_id);


    Streamer streamer(grid);
#pragma omp parallel for
            /* for(unsigned int ix=sX; ix<eX; ix++) */
        /* for(unsigned int iy=sY; iy<eY; iy++) */
    /* for(unsigned int iz=sZ; iz<eZ; iz++) */
    for(unsigned int iz=sZ; iz<eZ; iz++)
        for(unsigned int iy=sY; iy<eY; iy++)
            for(unsigned int ix=sX; ix<eX; ix++)
            {
                /* const unsigned int idx = NCHANNELS * (iz + NZ * (iy + NY * ix)); */
                const unsigned int idx = NCHANNELS * (ix + NX * (iy + NY * iz));
                assert(idx < NX * NY * NZ * NCHANNELS);

                Real * const ptr = array_all + idx;

                Real output[NCHANNELS];
                for(int i=0; i<NCHANNELS; ++i)
                    output[i] = 0;

                streamer.operate(ix, iy, iz, (Real *)output);

                for(int i=0; i<NCHANNELS; ++i)
                    ptr[i] = output[i];
            }

    fapl_id = H5Pcreate(H5P_DATASET_XFER);
    H5Pset_dxpl_mpio(fapl_id, H5FD_MPIO_COLLECTIVE);

    fspace_id = H5Screate_simple(4, dims, NULL);
    dataset_id = H5Dcreate(file_id, "data", HDF_REAL, fspace_id, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);

    fspace_id = H5Dget_space(dataset_id);
    H5Sselect_hyperslab(fspace_id, H5S_SELECT_SET, offset, NULL, count, NULL);
    mspace_id = H5Screate_simple(4, count, NULL);
    status = H5Dwrite(dataset_id, HDF_REAL, mspace_id, fspace_id, fapl_id, array_all);

    status = H5Sclose(mspace_id);
    status = H5Sclose(fspace_id);
    status = H5Dclose(dataset_id);
    status = H5Pclose(fapl_id);
    status = H5Fclose(file_id);
    H5close();

    delete [] array_all;

    if (rank==0)
    {
        char wrapper[256];
        sprintf(wrapper, "%s/%s.xmf", dump_path.c_str(), f_name.c_str());
        FILE *xmf = 0;
        xmf = fopen(wrapper, "w");
        fprintf(xmf, "<?xml version=\"1.0\" ?>\n");
        fprintf(xmf, "<!DOCTYPE Xdmf SYSTEM \"Xdmf.dtd\" []>\n");
        fprintf(xmf, "<Xdmf Version=\"2.0\">\n");
        fprintf(xmf, " <Domain>\n");
        fprintf(xmf, "   <Grid GridType=\"Uniform\">\n");
        fprintf(xmf, "     <Time Value=\"%05d\"/>\n", iCounter);
        fprintf(xmf, "     <Topology TopologyType=\"3DCORECTMesh\" Dimensions=\"%d %d %d\"/>\n", (int)dims[0], (int)dims[1], (int)dims[2]);
        fprintf(xmf, "     <Geometry GeometryType=\"ORIGIN_DXDYDZ\">\n");
        fprintf(xmf, "       <DataItem Name=\"Origin\" Dimensions=\"3\" NumberType=\"Float\" Precision=\"4\" Format=\"XML\">\n");
        fprintf(xmf, "        %e %e %e\n", 0.,0.,0.);
        fprintf(xmf, "       </DataItem>\n");
        fprintf(xmf, "       <DataItem Name=\"Spacing\" Dimensions=\"3\" NumberType=\"Float\" Precision=\"4\" Format=\"XML\">\n");
        fprintf(xmf, "        %e %e %e\n", grid.getH(), grid.getH(), grid.getH());
        fprintf(xmf, "       </DataItem>\n");
        fprintf(xmf, "     </Geometry>\n");

        fprintf(xmf, "     <Attribute Name=\"data\" AttributeType=\"%s\" Center=\"Node\">\n", Streamer::getAttributeName());
        fprintf(xmf, "       <DataItem Dimensions=\"%d %d %d %d\" NumberType=\"Float\" Precision=\"4\" Format=\"HDF\">\n", (int)dims[0], (int)dims[1], (int)dims[2], (int)dims[3]);
        fprintf(xmf, "        %s:/data\n",(f_name+".h5").c_str());
        fprintf(xmf, "       </DataItem>\n");
        fprintf(xmf, "     </Attribute>\n");

        fprintf(xmf, "   </Grid>\n");
        fprintf(xmf, " </Domain>\n");
        fprintf(xmf, "</Xdmf>\n");
        fclose(xmf);
    }
#else
#warning USE OF HDF WAS DISABLED AT COMPILE TIME
#endif
}

template<typename TGrid, typename Streamer>
void ReadHDF5_MPI(TGrid &grid, const string f_name, const string dump_path=".")
{
#ifdef _USE_HDF_

    int rank;
    char filename[256];
    herr_t status;
    hid_t file_id, dataset_id, fspace_id, fapl_id, mspace_id;

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    int coords[3];
    grid.peindex(coords);

    const unsigned int NX = TGrid::sizeX;
    const unsigned int NY = TGrid::sizeY;
    const unsigned int NZ = TGrid::sizeZ;
    static const unsigned int NCHANNELS = Streamer::NCHANNELS;

    Real * array_all = new Real[NX * NY * NZ * NCHANNELS];

    static const int sX = 0;
    static const int sY = 0;
    static const int sZ = 0;

    const int eX = TGrid::sizeX;
    const int eY = TGrid::sizeY;
    const int eZ = TGrid::sizeZ;

    hsize_t count[4] = {
        TGrid::sizeZ,
        TGrid::sizeY,
        TGrid::sizeX, NCHANNELS};

    hsize_t dims[4] = {
        grid.getBlocksPerDimension(2)*TGrid::sizeZ,
        grid.getBlocksPerDimension(1)*TGrid::sizeY,
        grid.getBlocksPerDimension(0)*TGrid::sizeX, NCHANNELS};

    hsize_t offset[4] = {
        coords[2]*TGrid::sizeZ,
        coords[1]*TGrid::sizeY,
        coords[0]*TGrid::sizeX, 0};

    sprintf(filename, "%s/%s.h5", dump_path.c_str(), f_name.c_str());

    H5open();
    fapl_id = H5Pcreate(H5P_FILE_ACCESS);
    status = H5Pset_fapl_mpio(fapl_id, MPI_COMM_WORLD, MPI_INFO_NULL);
    file_id = H5Fopen(filename, H5F_ACC_RDONLY, fapl_id);
    status = H5Pclose(fapl_id);

    dataset_id = H5Dopen2(file_id, "data", H5P_DEFAULT);
    fapl_id = H5Pcreate(H5P_DATASET_XFER);
    H5Pset_dxpl_mpio(fapl_id, H5FD_MPIO_COLLECTIVE);

    fspace_id = H5Dget_space(dataset_id);
    H5Sselect_hyperslab(fspace_id, H5S_SELECT_SET, offset, NULL, count, NULL);

    mspace_id = H5Screate_simple(4, count, NULL);
    status = H5Dread(dataset_id, HDF_REAL, mspace_id, fspace_id, fapl_id, array_all);

    Streamer streamer(grid);
    #pragma omp parallel for
        for(int iz=sZ; iz<eZ; iz++)
            for(int iy=sY; iy<eY; iy++)
                for(int ix=sX; ix<eX; ix++)
                {
                    Real * const ptr_input = array_all + NCHANNELS*(ix + NX * (iy + NY * iz));

                    streamer.operate(ptr_input, ix, iy, iz);
                }

    status = H5Pclose(fapl_id);
    status = H5Dclose(dataset_id);
    status = H5Sclose(fspace_id);
    status = H5Sclose(mspace_id);
    status = H5Fclose(file_id);

    H5close();

    delete [] array_all;
#else
#warning USE OF HDF WAS DISABLED AT COMPILE TIME
#endif
}
