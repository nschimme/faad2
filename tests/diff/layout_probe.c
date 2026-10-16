/* Compile against the stock 2.11.4 header to regenerate layout constants. */
#include <neaacdec.h>
#include <stddef.h>
#include <stdio.h>
#define S(t) printf("    EQ(sizeof(" #t "), %zu);\n", sizeof(t))
#define O(t,f) printf("    EQ(offsetof(" #t ", " #f "), %zu);\n",offsetof(t,f))
int main(void) {
S(NeAACDecConfiguration); O(NeAACDecConfiguration,defObjectType); O(NeAACDecConfiguration,defSampleRate); O(NeAACDecConfiguration,outputFormat); O(NeAACDecConfiguration,downMatrix); O(NeAACDecConfiguration,useOldADTSFormat); O(NeAACDecConfiguration,dontUpSampleImplicitSBR);
S(NeAACDecFrameInfo); O(NeAACDecFrameInfo,bytesconsumed); O(NeAACDecFrameInfo,samples); O(NeAACDecFrameInfo,channels); O(NeAACDecFrameInfo,error); O(NeAACDecFrameInfo,samplerate); O(NeAACDecFrameInfo,sbr); O(NeAACDecFrameInfo,object_type); O(NeAACDecFrameInfo,header_type); O(NeAACDecFrameInfo,num_front_channels); O(NeAACDecFrameInfo,num_side_channels); O(NeAACDecFrameInfo,num_back_channels); O(NeAACDecFrameInfo,num_lfe_channels); O(NeAACDecFrameInfo,channel_position); O(NeAACDecFrameInfo,ps);
S(mp4AudioSpecificConfig); O(mp4AudioSpecificConfig,objectTypeIndex); O(mp4AudioSpecificConfig,samplingFrequencyIndex); O(mp4AudioSpecificConfig,samplingFrequency); O(mp4AudioSpecificConfig,channelsConfiguration); O(mp4AudioSpecificConfig,frameLengthFlag); O(mp4AudioSpecificConfig,dependsOnCoreCoder); O(mp4AudioSpecificConfig,coreCoderDelay); O(mp4AudioSpecificConfig,extensionFlag); O(mp4AudioSpecificConfig,aacSectionDataResilienceFlag); O(mp4AudioSpecificConfig,aacScalefactorDataResilienceFlag); O(mp4AudioSpecificConfig,aacSpectralDataResilienceFlag); O(mp4AudioSpecificConfig,epConfig); O(mp4AudioSpecificConfig,sbr_present_flag); O(mp4AudioSpecificConfig,forceUpSampling); O(mp4AudioSpecificConfig,downSampledSBR); O(mp4AudioSpecificConfig,ps_present_flag);
}
