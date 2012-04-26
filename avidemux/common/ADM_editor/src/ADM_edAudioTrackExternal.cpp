/***************************************************************************
    \file  ADM_edAudioTrackExternal
    \brief Manage audio track(s) coming from an external file
    \author (c) 2012 Mean, fixounet@free.Fr 
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <string.h>
#include "ADM_cpp.h"
#include "ADM_default.h"
#include <math.h>


#include "fourcc.h"
#include "ADM_edit.hxx"
#include "ADM_edAudioTrackExternal.h"
#include "ADM_audioIdentify.h"
#include "ADM_audioAccessFile.h"
#include "ADM_vidMisc.h"
#if 1
#define vprintf ADM_info
#else
#define vprintf(...) {}
#endif
/**
    \fn ctor
*/
ADM_edAudioTrackExternal:: ADM_edAudioTrackExternal(const char *file, WAVHeader *hdr,ADM_audioAccess *cess)
:  ADM_edAudioTrack(ADM_EDAUDIO_EXTERNAL,hdr,cess)
{
    ADM_info("Creating edAudio from external file %s\n",file);
    sourceFile=std::string(file);
    codec=NULL;
    vbr=false;
    duration=0;
    size=0;

}
/**
    \fn dtor
*/
ADM_edAudioTrackExternal::~ADM_edAudioTrackExternal()
{
    ADM_info("Destroying edAudio from external %s \n",sourceFile.c_str());
    if(codec) delete codec;
    codec=NULL;
    
}
/**
    \fn create
    \brief actually create the track, can fail
*/
bool ADM_edAudioTrackExternal::create(void)
{
    ADM_info("Initializing audio track from external %s \n",sourceFile.c_str());
    codec=getAudioCodec(wavHeader.encoding,&wavHeader,0,NULL);;
    size=access->getLength();
    return true;
}
/**
    \fn getChannelMapping
*/
CHANNEL_TYPE * ADM_edAudioTrackExternal::getChannelMapping(void )
{
    return codec->channelMapping;
}
/**
    \fn getOutputFrequency
*/          
uint32_t    ADM_edAudioTrackExternal::getOutputFrequency(void)
{
    return codec->getOutputFrequency();
}
/**
    \fn refillPacketBuffer
*/
bool             ADM_edAudioTrackExternal::refillPacketBuffer(void)
{
   packetBufferSize=0; 
   uint64_t dts;
 
    if(!getPacket(packetBuffer,&packetBufferSize,ADM_EDITOR_PACKET_BUFFER_SIZE,
                        &packetBufferSamples,&dts))
    {           
             return false;
    }
    //
    // Ok we have a packet, rescale audio
    if(dts==ADM_NO_PTS) packetBufferDts=ADM_NO_PTS;
    return true;
}
/**
    \fn create_edAudioExternal
*/
ADM_edAudioTrackExternal *create_edAudioExternal(const char *name)
{
    #define EXTERNAL_PROBE_SIZE (1024*1024)
    // Identify file type
    uint8_t buffer[EXTERNAL_PROBE_SIZE];
    FILE *f=ADM_fopen(name,"rb");
    if(!f)
    {
        ADM_warning("Cannot open %s\n",name);
        return NULL;
    }
    fread(buffer,1,EXTERNAL_PROBE_SIZE,f);
    fclose(f);
    WAVHeader hdr;
    if(false==ADM_identifyAudioStream(EXTERNAL_PROBE_SIZE,buffer,hdr))
    {
        ADM_warning("Cannot identify external audio track\n");
        return NULL;
    }
    // Try to create an access for the file...
    switch(hdr.encoding)
    {
        case WAV_PCM:
        case WAV_AC3:
        case WAV_MP3:
                break;
        default:
                ADM_warning("Unsupported external audio tracks \n");
                return NULL;
                break;
    }
    // create access
    ADM_audioAccessFile *access=new ADM_audioAccessFile(name);
    // create ADM_edAudioTrack
    ADM_edAudioTrackExternal *external=new ADM_edAudioTrackExternal(name, &hdr,access);
    if(!external->create())
    {
        delete external;
        external=NULL;
        ADM_warning("Failed to create external track from %s\n",name);
        return NULL;
    }
    // done!
    return external;
}

/**
    \fn getPCMPacket
*/
bool         ADM_edAudioTrackExternal::getPCMPacket(float  *dest, uint32_t sizeMax, uint32_t *samples,uint64_t *odts)
{
uint32_t fillerSample=0;   // FIXME : Store & fix the DTS error correctly!!!!
uint32_t inSize;
bool      drop=false;
uint32_t outFrequency=codec->getOutputFrequency();
 vprintf("[PCMPacketExt]  request TRK %d:%x\n",0,(long int)0);
again:
    *samples=0;
    // Do we already have a packet ?
    if(!packetBufferSize)
    {
        if(!refillPacketBuffer())
        {
            ADM_warning("Cannot refill\n");
            return false;
        }
    }
    // We do now
    vprintf("[PCMPacketExt]  TRK %d Got %d samples, time code %08lu  lastDts=%08lu delta =%08ld\n",
                0,packetBufferSamples,packetBufferDts,lastDts,packetBufferDts-lastDts);


    // If lastDts is not initialized....
    if(lastDts==ADM_AUDIO_NO_DTS) lastDts=packetBufferDts;
    
    //
    //  The packet is ok, decode it...
    //
    uint32_t nbOut=0; // Nb sample as seen by codec
    if(!codec->run(packetBuffer, packetBufferSize, dest, &nbOut))
    {
            packetBufferSize=0; // consume
            ADM_warning("[PCMPacketExt::getPCMPacket] Track %d:%x : codec failed failed\n", 0,0);
            return false;
    }
    packetBufferSize=0; // consume

    // Compute how much decoded sample to compare with what demuxer said
    uint32_t decodedSample=nbOut;
    decodedSample/=wavHeader.channels;
    if(!decodedSample) goto again;
#define ADM_MAX_JITTER 5000  // in samples, due to clock accuracy, it can be +er, -er, + er, -er etc etc
    if(abs(decodedSample-packetBufferSamples)>ADM_MAX_JITTER)
    {
        ADM_warning("[PCMPacketExt::getPCMPacket] Track %d:%x Demuxer was wrong %d vs %d samples!\n",
                    0,0,packetBufferSamples,decodedSample);
    }
    
    // Update infos
    *samples=(decodedSample);
    *odts=lastDts;
    advanceDtsByCustomSample(decodedSample,outFrequency);
    vprintf("[Composer::getPCMPacket] Track %d:%x Adding %u decoded, Adding %u filler sample, dts is now %lu\n",
                    0,(long int)0,  decodedSample,fillerSample,lastDts);
    ADM_assert(sizeMax>=(fillerSample+decodedSample)*wavHeader.channels);
    vprintf("[getPCMext] %d samples, dts=%s\n",*samples,ADM_us2plain(*odts));
    return true;
}
// EOF