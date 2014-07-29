/*
 *  Profiler.h
 *  Cubism
 *
 *  Created by Diego Rossinelli on 9/13/08.
 *  Copyright 2008 CSE Lab, ETH Zurich. All rights reserved.
 *
 */
#pragma once

#include <assert.h>
#undef min
#undef max
#include <vector>
#undef min
#undef max
#include <map>
#include <string>
#include <stdio.h>
#include <stack>

using namespace std;

#include <sys/time.h>
//#include <tbb/tick_count.h>
//namespace tbb { class tick_count; }

const bool bVerboseProfiling = false;

enum {CPU, CUDA};

class ProfileAgent
{
//  typedef tbb::tick_count ClockTime;
    typedef timeval ClockTime;


    ClockTime m_tStart, m_tEnd;

    static void _getTime(ClockTime& time);

    static double _getElapsedTime(const ClockTime& tS, const ClockTime& tE);

protected:

    enum ProfileAgentState{ ProfileAgentState_Created, ProfileAgentState_Started, ProfileAgentState_Stopped};
    ProfileAgentState m_state;
    long double m_dAccumulatedTime;
    int m_nMeasurements;
    int m_nMoney;

    virtual void _reset()
    {
        m_tStart = ClockTime();
        m_tEnd = ClockTime();
        m_dAccumulatedTime = 0;
        m_nMeasurements = 0;
        m_nMoney = 0;
        m_state = ProfileAgentState_Created;
    }

public:

    ProfileAgent():m_tStart(), m_tEnd(), m_state(ProfileAgentState_Created),
    m_dAccumulatedTime(0), m_nMeasurements(0), m_nMoney(0) {}
    virtual ~ProfileAgent() {}

    virtual void start()
    {
        assert(m_state == ProfileAgentState_Created || m_state == ProfileAgentState_Stopped);

        if (bVerboseProfiling) {printf("start\n");}

        _getTime(m_tStart);

        m_state = ProfileAgentState_Started;
    }

    virtual void stop(int nMoney=0)
    {
        assert(m_state == ProfileAgentState_Started);

        if (bVerboseProfiling) {printf("stop\n");}

        _getTime(m_tEnd);
        m_dAccumulatedTime += _getElapsedTime(m_tStart, m_tEnd);
        m_nMeasurements++;
        m_nMoney += nMoney;
        m_state = ProfileAgentState_Stopped;
    }

    friend class Profiler;
};


class ProfileAgentCUDA : public ProfileAgent
{
    static void _getTime(const void *event, const void *stream);
    static double _getElapsedTime(const void *tStart, const void *tEnd);
    void _createEvent(void **event);
    void _destroyEvent(void *event);

    void *m_tStart, *m_tEnd;
    const void *m_Stream;

protected:

    virtual void _reset()
    {
        ProfileAgent::_reset();
        _createEvent(&m_tStart);
        _createEvent(&m_tEnd);
        m_Stream = NULL;
    }

public:

    ProfileAgentCUDA(const void *stream_=NULL):ProfileAgent(), m_tStart(NULL), m_tEnd(NULL), m_Stream(stream_)
    {
        _createEvent(&m_tStart);
        _createEvent(&m_tEnd);
    }

    virtual ~ProfileAgentCUDA()
    {
        _destroyEvent(m_tStart);
        _destroyEvent(m_tEnd);
    }

    virtual void start()
    {
        assert(m_state == ProfileAgentState_Created || m_state == ProfileAgentState_Stopped);

        if (bVerboseProfiling) {printf("start\n");}

        _getTime(m_tStart, m_Stream);

        m_state = ProfileAgentState_Started;
    }

    virtual void stop(int nMoney=0)
    {
        assert(m_state == ProfileAgentState_Started);

        if (bVerboseProfiling) {printf("stop\n");}

        _getTime(m_tEnd, m_Stream);
        m_dAccumulatedTime += _getElapsedTime(m_tStart, m_tEnd);
        m_nMeasurements++;
        m_nMoney += nMoney;
        m_state = ProfileAgentState_Stopped;
    }

    friend class Profiler;
};


struct ProfileSummaryItem
{
    string sName;
    double dTime;
    double dAverageTime;
    int nMoney;
    int nSamples;

    ProfileSummaryItem(string sName_, double dTime_, int nMoney_, int nSamples_):
        sName(sName_), dTime(dTime_), nMoney(nMoney_),nSamples(nSamples_), dAverageTime(dTime_/nSamples_){}
};


class Profiler
{
protected:

    map<string, ProfileAgent*> m_mapAgents;
    stack<string> m_mapStoppedAgents;

    ProfileAgent& _getAgent(string sName, const int type=CPU, const void *stream=NULL)
    {
        if (bVerboseProfiling) {printf("%s ", sName.data());}

        map<string, ProfileAgent*>::const_iterator it = m_mapAgents.find(sName);

        const bool bFound = it != m_mapAgents.end();

        if (bFound) return *it->second;

        ProfileAgent *agent;
        switch (type)
        {
            case CPU:  agent = new ProfileAgent(); break;
            case CUDA: agent = new ProfileAgentCUDA(stream); break;
        }

        m_mapAgents[sName] = agent;

        return *agent;
    }

    vector<ProfileSummaryItem> _createSummary(bool bSkipIrrelevantEntries=true) const
    {
        vector<ProfileSummaryItem> result;
        result.reserve(m_mapAgents.size());

        for(map<string, ProfileAgent*>::const_iterator it = m_mapAgents.begin(); it != m_mapAgents.end(); it++)
        {
            const ProfileAgent& agent = *it->second;
            if (!bSkipIrrelevantEntries || agent.m_dAccumulatedTime>1e-3)
                result.push_back(ProfileSummaryItem(it->first, agent.m_dAccumulatedTime, agent.m_nMoney, agent.m_nMeasurements));
        }

        return result;
    }


public:

    void clear()
    {
        for(map<string, ProfileAgent*>::iterator it = m_mapAgents.begin(); it != m_mapAgents.end(); it++)
        {
            delete it->second;

            it->second = NULL;
        }

        m_mapAgents.clear();
    }

    Profiler(): m_mapAgents(){}

    ~Profiler()
    {
        clear();
    }

#if !defined(_PROFILE_NONE_) && defined(_PROFILE_CUDA_)
    // Timing CUDA means cudaEventSynchronize calls, which we don't want in
    // production code. Enable profiling of CUDA kernels with this flag.
    void push_startCUDA(string sAgentName, const void *stream=NULL)
    {
        if (m_mapStoppedAgents.size() > 0)
            _getAgent(m_mapStoppedAgents.top()).stop();

        m_mapStoppedAgents.push(sAgentName);
        _getAgent(m_mapStoppedAgents.top(), CUDA, stream).start();
    }

    inline void pop_stopCUDA() { pop_stop(); }
#else
    inline void push_startCUDA(string sAgentName, const void *stream=NULL) { }
    inline void pop_stopCUDA() { }
#endif

#ifndef _PROFILE_NONE_
    void push_start(string sAgentName)
    {
        if (m_mapStoppedAgents.size() > 0)
            _getAgent(m_mapStoppedAgents.top()).stop();

        m_mapStoppedAgents.push(sAgentName);
        _getAgent(m_mapStoppedAgents.top(), CPU, NULL).start();
    }

    void pop_stop()
    {
        string sCurrentAgentName = m_mapStoppedAgents.top();
        _getAgent(sCurrentAgentName).stop();
        m_mapStoppedAgents.pop();

        if (m_mapStoppedAgents.size() == 0) return;

        _getAgent(m_mapStoppedAgents.top()).start();
    }

    void printSummary(FILE *outFile=NULL) const
    {
        vector<ProfileSummaryItem> v = _createSummary();

        double dTotalTime = 0;
        double dTotalTime2 = 0;
        for(vector<ProfileSummaryItem>::const_iterator it = v.begin(); it!= v.end(); it++)
            dTotalTime += it->dTime;

        for(vector<ProfileSummaryItem>::const_iterator it = v.begin(); it!= v.end(); it++)
        dTotalTime2 += it->dTime - it->nSamples*1.30e-6;

        for(vector<ProfileSummaryItem>::const_iterator it = v.begin(); it!= v.end(); it++)
        {
            const ProfileSummaryItem& item = *it;
            const double avgTime = item.dAverageTime;

            printf("[%15s]: \t%02.0f-%02.0f%%\t%03.3e (%03.3e) s\t%03.3f (%03.3f) s\t(%d samples)\n",
                   item.sName.data(), 100*item.dTime/dTotalTime, 100*(item.dTime- item.nSamples*1.3e-6)/dTotalTime2, avgTime,avgTime-1.30e-6,  item.dTime, item.dTime- item.nSamples*1.30e-6, item.nSamples);
            if (outFile) fprintf(outFile,"[%15s]: \t%02.2f%%\t%03.3f s\t(%d samples)\n",

                   item.sName.data(), 100*item.dTime/dTotalTime, avgTime, item.nSamples);
        }

        printf("[Total time]: \t%f\n", dTotalTime);
        if (outFile) fprintf(outFile,"[Total time]: \t%f\n", dTotalTime);
        if (outFile) fflush(outFile);
        if (outFile) fclose(outFile);
    }

    void reset()
    {
        printf("reset\n");
        for(map<string, ProfileAgent*>::const_iterator it = m_mapAgents.begin(); it != m_mapAgents.end(); it++)
            it->second->_reset();
    }

#else
    inline void push_start(string sAgentName) { }
    inline void pop_stop() { }
    inline void printSummary(FILE *outFile=NULL) const { }
    inline void reset() { }
#endif

    friend class ProfileAgent;
};