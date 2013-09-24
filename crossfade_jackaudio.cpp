//
//  crossfade_jackaudio.cpp
//  
//
//  Created by Sarah Denoux on 15/07/13.
//  Copyright (c) 2013 __MyCompanyName__. All rights reserved.
//

#include "crossfade_jackaudio.h"

/******************************************************************************
 *******************************************************************************
 
 JACK AUDIO WITH CROSSFADE
 
 *******************************************************************************
 *******************************************************************************/

float crossfade_jackaudio::crossfade_calculation(int i, int j){
    bool connectFadeOut = false;
    bool connectFadeIn = false;
    
    list<pair<string, string> >::const_iterator it;
    
    for (it = fConnections.begin(); it != fConnections.end(); it++){
        
        string jackPort(jack_port_name(fOutputPorts[j]));
        
        if(jackPort.compare(it->first) == 0){
            connectFadeOut = true;
            break;
        }
    }
    for (it = connectionsIn.begin(); it != connectionsIn.end(); it++){
        
        string jackPort(jack_port_name(fOutputPorts[j]));
        
        if(jackPort.compare(it->first) == 0){
            connectFadeIn = true;
            break;
        }
    }
    
    if(connectFadeIn && connectFadeOut)
        return (fIntermediateFadeIn[j][i]*InCoef) + (fIntermediateFadeOut[j][i]/OutCoef);
    else if(connectFadeIn)
        return (fIntermediateFadeIn[j][i]*InCoef);
    else if(connectFadeOut)
        return (fIntermediateFadeOut[j][i]/OutCoef);
    else
        return 0;
}


crossfade_jackaudio::crossfade_jackaudio(const void* icon_data = 0, size_t icon_size = 0)
{
    fDsp = NULL;
    fClient = NULL;
    fNumInChans = 0;
    fNumOutChans = 0;
    fInputPorts = NULL;
    fOutputPorts = NULL;
    fShutdown = 0;
    fShutdownArg = 0;
    
    fInputPorts = new jack_port_t*[256];
    fOutputPorts = new jack_port_t*[256];
    
    doWeFadeOut = false;
    InCoef = 0.01;
    OutCoef = 1.0;
    NumberOfFadeProcess = 0;
    
    if (icon_data) {
        fIconData = malloc(icon_size);
        fIconSize = icon_size;
        memcpy(fIconData, icon_data, icon_size);
    } else {
        fIconData = NULL;
        fIconSize = 0;
    }
}

crossfade_jackaudio::~crossfade_jackaudio() 
{ 
    //stop(); 
    if (fIconData) {
        free(fIconData);
    }
}

bool crossfade_jackaudio::init(const char* name, dsp* DSP){ return false;} 

bool crossfade_jackaudio::init(const char* name, dsp* DSP, const char* portsName) 
{
    fDsp = DSP;
    
    if ((fClient = jack_client_open(name, JackNullOption, NULL)) == 0) {
        fprintf(stderr, "JACK server not running ?\n");
        return false;
    }
#ifdef JACK_IOS
    jack_custom_publish_data(fClient, "icon.png", fIconData, fIconSize);
#endif
    
#ifdef _OPENMP
    jack_set_process_thread(fClient, _jack_thread, this);
#else
    jack_set_process_callback(fClient, _jack_process, this);
#endif
    
    jack_set_sample_rate_callback(fClient, _jack_srate, this);
    jack_on_info_shutdown(fClient, _jack_info_shutdown, this);
    
    fNumInChans  = fDsp->getNumInputs();
    fNumOutChans = fDsp->getNumOutputs();
    
    bufferSize = jack_get_buffer_size(fClient);
    
    //        fInput_ports = new jack_port_t*[fNumInChans];
    //      fOutput_ports = new jack_port_t*[fNumOutChans];
    
    for (int i = 0; i < fNumInChans; i++) {
        char buf[256];
        snprintf(buf, 256, "%s_In_%d",portsName, i);
        fInputPorts[i] = jack_port_register(fClient, buf, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    }
    for (int i = 0; i < fNumOutChans; i++) {
        char buf[256];
        snprintf(buf, 256, "%s_Out_%d",portsName, i);
        fOutputPorts[i] = jack_port_register(fClient, buf, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    }
    
    portName = portsName;
    
    fDsp->init(jack_get_sample_rate(fClient));
    return true;
}

bool crossfade_jackaudio::start(){
    if (jack_activate(fClient)) {
        fprintf(stderr, "Cannot activate client");
        return false;
    }
    return true;
}

void crossfade_jackaudio::init_FadeIn_Audio(dsp* DSP, const char* portsName){
    fDspIn = DSP;
    
    fNumInDspFade  = fDspIn->getNumInputs();
    fNumOutDspFade = fDspIn->getNumOutputs();  
    
    //Rename the common ports
    for(int i = 0; i<fNumInChans; i++){
        char buf[256];
        snprintf(buf, 256, "%s_In_%d",portsName, i);
        
        jack_port_set_name (fInputPorts[i], buf);
    }
    for(int i = 0; i<fNumOutChans; i++){
        char buf[256];
        snprintf(buf, 256, "%s_Out_%d",portsName, i);
        jack_port_set_name (fOutputPorts[i], buf);
    }
    
    //Register the new ports 
    if(fNumInChans<fNumInDspFade){
        
        for (int i = fNumInChans; i < fNumInDspFade; i++) {
            char buf[256];
            snprintf(buf, 256, "%s_In_%d", portsName,i);
            
            fInputPorts[i] = jack_port_register(fClient, buf, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        }
    }
    
    if(fNumOutChans<fNumOutDspFade){
        for (int i = fNumOutChans; i < fNumOutDspFade; i++) {
            char buf[256];
            snprintf(buf, 256, "%s_Out_%d",portsName, i);
            fOutputPorts[i] = jack_port_register(fClient, buf, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        }
    }
    
    fDspIn->init(jack_get_sample_rate(fClient));
    save_connections();
    connectionsIn = fConnections;
}

int crossfade_jackaudio::reconnect(list<pair<string, string> > Connections)
{        
    list<pair<string, string> >::const_iterator it;
    
    int returning = 0;
    
    for (it = Connections.begin(); it != Connections.end(); it++){
        int result = jack_connect(fClient, it->first.c_str(), it->second.c_str());
        if(result != 0)
            returning = result;
    }
    
    return returning;
}

// UpDate the list of ports needed by new DSP
void crossfade_jackaudio::launch_fadeOut()
{
    //Allocation of the intermediate buffers needed for the crossfade
    fIntermediateFadeOut = new float*[fNumOutChans];
    for(int i=0; i<fNumOutChans; i++)
        fIntermediateFadeOut[i] = new float[bufferSize];
    
    fIntermediateFadeIn = new float*[fNumOutDspFade];
    for(int i=0; i<fNumOutDspFade;i++)
        fIntermediateFadeIn[i] = new float[bufferSize];
    
    set_doWeFadeOut(true); 
}

void crossfade_jackaudio::launch_fadeIn(){}
bool crossfade_jackaudio::get_FadeOut(){return get_doWeFadeOut();}

// The inFading DSP becomes the current one
void crossfade_jackaudio::upDate_DSP(){
    
    //Erase the extra ports
    if(fNumInChans>fNumInDspFade)
    {
        for (int i = fNumInDspFade; i < fNumInChans; i++){
            jack_port_unregister(fClient, fInputPorts[i]);
        }
    }
    if(fNumOutChans>fNumOutDspFade)
    {
        for (int i = fNumOutDspFade; i < fNumOutChans; i++){
            jack_port_unregister(fClient, fOutputPorts[i]);
        }
    }
    
    fNumInChans = fNumInDspFade;
    fNumOutChans = fNumOutDspFade;
    
    fDsp = fDspIn;
    
    delete [] fIntermediateFadeOut;
    delete [] fIntermediateFadeIn;
}

// jack callbacks
int	crossfade_jackaudio::process(jack_nframes_t nframes) 
{
    AVOIDDENORMALS;
    // Retrieve JACK inputs/output audio buffers
    
    float* fInChannel[fNumInChans];
    
    for (int i = 0; i < fNumInChans; i++) {
        fInChannel[i] = (float*)jack_port_get_buffer(fInputPorts[i], nframes);
    }
    
    if(doWeFadeOut){
        
        //Step 1 : Calculation of intermediate buffers
        
        fDsp->compute(nframes, fInChannel, fIntermediateFadeOut);
        
        float* fInChannelDspIn[fNumInDspFade];
        
        for (int i = 0; i < fNumInDspFade; i++) {
            fInChannelDspIn[i] = (float*)jack_port_get_buffer(fInputPorts[i], nframes);
        }
        fDspIn->compute(nframes, fInChannelDspIn, fIntermediateFadeIn); 
        
        int fNumOutPorts = max(fNumOutChans, fNumOutDspFade);
        float* fOutFinal[fNumOutPorts];
        
        //Step 2 : Mixing the 2 DSP in the final output buffer taking into account the number of IN/OUT ports of the in- and out-coming DSP
        
        for (int i = 0; i < fNumOutPorts; i++) 
            fOutFinal[i] = (float*)jack_port_get_buffer(fOutputPorts[i], nframes); 
        
        if(fNumOutChans<fNumOutDspFade){
            for (int i = 0; i < nframes; i++) {
                for (int j = 0; j < fNumOutChans; j++)
                    fOutFinal[j][i] = crossfade_calculation(i, j);
                
                for (int j = fNumOutChans; j < fNumOutDspFade; j++)
                    fOutFinal[j][i] = fIntermediateFadeIn[j][i]*InCoef;
                
                if(InCoef < 1)
                    InCoef = InCoef*FadeInCoefficient;  
                OutCoef = OutCoef*FadeOutCoefficient;  
            }
        } 
        
        else{
            for (int i = 0; i < nframes; i++) {
                for (int j = 0; j < fNumOutDspFade; j++)
                    fOutFinal[j][i] = crossfade_calculation(i, j);
                
                for (int j = fNumOutDspFade; j < fNumOutChans; j++)
                    fOutFinal[j][i] = fIntermediateFadeOut[j][i]/OutCoef;
                
                if(InCoef < 1)   
                    InCoef = InCoef*FadeInCoefficient;  
                OutCoef = OutCoef*FadeOutCoefficient;        
            }   
        }
        increment_crossFade();
    }
    
    //Normal processing
    else{
        float* fOutFinal[fNumOutChans];
        
        for (int i = 0; i < fNumOutChans; i++)
            fOutFinal[i] = (float*)jack_port_get_buffer(fOutputPorts[i], nframes);
        
        fDsp->compute(nframes, fInChannel, fOutFinal);   
    }
    
    
    return 0;
}

// Access to the fade parameter
list<pair<string, string> > crossfade_jackaudio::get_audio_connections()
{
    save_connections();
    return fConnections;
}  

