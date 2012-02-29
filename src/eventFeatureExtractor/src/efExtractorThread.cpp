// -*- mode:C++; tab-width:4; c-basic-offset:4; indent-tabs-mode:nil -*-

/*
  * Copyright (C)2011  Department of Robotics Brain and Cognitive Sciences - Istituto Italiano di Tecnologia
  * Author:Francesco Rea
  * email: francesco.rea@iit.it
  * Permission is granted to copy, distribute, and/or modify this program
  * under the terms of the GNU General Public License, version 2 or any
  * later version published by the Free Software Foundation.
  *
  * A copy of the license can be found at
  * http://www.robotcub.org/icub/license/gpl.txt
  *
  * This program is distributed in the hope that it will be useful, but
  * WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
  * Public License for more details
*/

/**
 * @file efExtractorThread.cpp
 * @brief Implementation of the thread (see header efExtractorThread.h)
 */

#include <iCub/efExtractorThread.h>

#include <cxcore.h>
#include <cv.h>
#include <highgui.h>
#include <stdio.h>
#include <cstring>
#include <cassert>

using namespace yarp::os;
using namespace yarp::sig;
using namespace yarp::dev;
using namespace std;

#define DIM 10
#define THRATE 20
#define SHIFTCONST 100
#define RETINA_SIZE 128
#define FEATUR_SIZE 32
#define ADDRESS 0x40000
#define X_MASK 0x000000FE
#define X_MASK_DEC    254
#define X_SHIFT 1
#define Y_MASK 0x00007F00
#define Y_MASK_DEC  32512    
#define Y_SHIFT 8
#define POLARITY_MASK 0x00000001
#define POLARITY_SHIFT 0
#define CONST_DECREMENT 1

#define CHUNKSIZE 32768

//#define VERBOSE

inline int convertChar2Dec(char value) {
    if (value > 60)
        return value - 97 + 10;
    else
        return value - 48;
}

inline void copy_8u_C1R(ImageOf<PixelMono>* src, ImageOf<PixelMono>* dest) {
    int padding = src->getPadding();
    int channels = src->getPixelCode();
    int width = src->width();
    int height = src->height();
    unsigned char* psrc = src->getRawImage();
    unsigned char* pdest = dest->getRawImage();
    for (int r=0; r < height; r++) {
        for (int c=0; c < width; c++) {
            *pdest++ = (unsigned char) *psrc++;
        }
        pdest += padding;
        psrc += padding;
    }
}

inline void copy_8u_C3R(ImageOf<PixelRgb>* src, ImageOf<PixelRgb>* dest) {
    int padding = src->getPadding();
    int channels = src->getPixelCode();
    int width = src->width();
    int height = src->height();
    unsigned char* psrc = src->getRawImage();
    unsigned char* pdest = dest->getRawImage();
    for (int r=0; r < height; r++) {
        for (int c=0; c < width; c++) {
            *pdest++ = (unsigned char) *psrc++;
            *pdest++ = (unsigned char) *psrc++;
            *pdest++ = (unsigned char) *psrc++;
        }
        pdest += padding;
        psrc += padding;
    }
}

efExtractorThread::efExtractorThread() : RateThread(THRATE) {
    firstHalf =  true;
    resized   = false;
    idle      = false;

    count             = 0;
    countEvent        = 0;
    leftInputImage    = 0;
    rightInputImage   = 0;
    shiftValue        = 20;
    lastTimestampLeft = 0;

    bufferCopy  = (char*) malloc(CHUNKSIZE);
    bufferCopy2 = (char*) malloc(CHUNKSIZE);
}

efExtractorThread::~efExtractorThread() {
    free(bufferCopy);
}

bool efExtractorThread::threadInit() {
    printf("starting the thread.... \n");
    /* open ports */
    printf("opening ports.... \n");
    outLeftPort.open(getName("/left:o").c_str());
    outRightPort.open(getName("/right:o").c_str());
    outFeaLeftPort.open(getName("/feaLeft:o").c_str());
    outFeaRightPort.open(getName("/feaRight:o").c_str());
    inLeftPort.open(getName("/left:i").c_str());
    inRightPort.open(getName("/right:i").c_str());
    outEventPort.open(getName("/event:o").c_str());
    
    ebHandler = new eventBottleHandler();
    ebHandler->useCallback();
    ebHandler->open(getName("/retinaBottle:i").c_str());

    receivedBottle = new Bottle();

    cfConverter=new cFrameConverter();
    cfConverter->useCallback();
    cfConverter->setVERBOSE(VERBOSE);
    cfConverter->open(getName("/retina:i").c_str());

    int SIZE_OF_EVENT = CHUNKSIZE >> 3; // every event is composed by 4bytes address and 4 bytes timestamp
    monBufSize_b = SIZE_OF_EVENT * sizeof(struct aer);

    bufferFEA = (aer *)  malloc(monBufSize_b);
    if ( bufferFEA == NULL ) {
        printf("bufferFEA malloc failed \n");
    }
    else {
        printf("bufferFEA successfully created \n");
    }

    eventFeaBuffer = new AER_struct[CHUNKSIZE>>3];

    printf("allocating memory for the LUT \n");
    lut = new int[RETINA_SIZE * RETINA_SIZE * 5];
    printf("initialising memory for LUT \n");

    for(int i = 0; i < RETINA_SIZE * RETINA_SIZE * 5; i++) {
        lut[i] = -1;
        //printf("i: %d  value : %d \n",i,lut[i]);
    }
    printf("successfully initialised memory for LUT \n");
    
    /*opening the file of the mapping and creating the LUT*/
    pFile = fopen (mapURL.c_str(),"rb");    
    fout  = fopen ("lut.txt","w+");
    fdebug  = fopen ("./eventFeatureExtractor.dumpSet.txt","w+");
    if (pFile!=NULL) {
        long lSize;
        size_t result;        
        // obtain file size:
        fseek (pFile , 0 , SEEK_END);
        lSize = ftell (pFile);
        printf("dimension of the file %lu \n", lSize);
        rewind(pFile);
        // saving into the buffer
        char * buffer;
        buffer = (char*) malloc (sizeof(char) * lSize);
        //fputs ("fopen example",pFile);
        printf("The file was correctly opened \n");
        result = fread (buffer,1,lSize,pFile);        
        printf(" Saved the data of the file into the buffer lSize:%lu \n", lSize);
        // checking the content of the buffer
        long word = 0;
        long input = -1, output = -1;
        short x, y;
        countMap = 0;

        
    }
     
    leftInputImage      = new ImageOf<PixelMono>;
    leftOutputImage     = new ImageOf<PixelMono>;
    rightOutputImage    = new ImageOf<PixelMono>;
    leftFeaOutputImage  = new ImageOf<PixelMono>;
    rightFeaOutputImage = new ImageOf<PixelMono>;
    //leftInputImage->resize(FEATUR_SIZE, FEATUR_SIZE);
    leftInputImage->resize(RETINA_SIZE,RETINA_SIZE);
    leftOutputImage->resize(RETINA_SIZE,RETINA_SIZE);
    rightOutputImage->resize(RETINA_SIZE,RETINA_SIZE);
    leftFeaOutputImage->resize(FEATUR_SIZE,FEATUR_SIZE);
    rightFeaOutputImage->resize(FEATUR_SIZE,FEATUR_SIZE);
    
    int padding = leftInputImage->getPadding();
    //initialisation of the memory image
    unsigned char* pLeft     = leftInputImage->getRawImage();
    unsigned char* pLeftOut  = leftOutputImage->getRawImage();
    unsigned char* pRightOut = rightOutputImage->getRawImage();
    
    // assign 127 to all the location in image plane
    int rowsize = leftInputImage->getRowSize();
    for(int r = 0 ; r < RETINA_SIZE ; r++){
        for(int c = 0 ; c < RETINA_SIZE ; c++){
            *pLeft  = 127; pLeft++;
            *pLeftOut = 127; pLeftOut++;
            *pRightOut = 127; pRightOut++;
        }
        pLeft +=  padding;
        pLeftOut += padding;
        pRightOut += padding;
    }
    
    unsigned char* pFeaLeftOut = leftFeaOutputImage->getRawImage();
    unsigned char* pFeaRightOut = rightFeaOutputImage->getRawImage();
    memset(pFeaLeftOut,  127, FEATUR_SIZE * FEATUR_SIZE * sizeof(unsigned char));
    memset(pFeaRightOut, 127, FEATUR_SIZE * FEATUR_SIZE * sizeof(unsigned char));
    
    printf("initialisation correctly ended \n");
    return true;
}

void efExtractorThread::interrupt() {
    outLeftPort.interrupt();
    outRightPort.interrupt();
    outFeaLeftPort.interrupt();
    outFeaRightPort.interrupt();
    inLeftPort.interrupt();
    inRightPort.interrupt();
    outEventPort.interrupt();
}

void efExtractorThread::threadRelease() {
    /* closing the ports*/
    printf("closing the ports \n");
    outLeftPort.close();
    outRightPort.close();
    outFeaLeftPort.close();
    outFeaRightPort.close();
    inLeftPort.close();
    inRightPort.close();
    outEventPort.close();

    printf("deleting the buffer of events \n");
    delete[] eventFeaBuffer;
    delete bufferFEA;
    delete cfConverter;
    delete receivedBottle;
    //delete bufferCopy;
    //delete bufferCopy2;
    
    /* closing the file */
    printf("deleting LUT \n");
    delete[] lut;
    
    printf("closing the file \n");
    fclose (pFile);

    printf("efExtractorThread::threadReleas : success in releasing \n ");
}

void efExtractorThread::setName(string str) {
    this->name=str;
    printf("name: %s", name.c_str());
}

std::string efExtractorThread::getName(const char* p) {
    string str(name);
    str.append(p);
    return str;
}

void efExtractorThread::resize(int widthp, int heightp) {
    width = widthp;
    height = heightp;
    //leftInputImage = new ImageOf<PixelMono>;
    //leftInputImage->resize(width, height);
    //rightInputImage = new ImageOf<PixelMono>;
    //rightInputImage->resize(width, height);
}


void efExtractorThread::generateMemory(int countEvent, int& countEventToSend) {
    int countEventLeft  = 0;
    int countEventRight = 0;
    //storing the response in a temp. image
    AER_struct* iterEvent = eventFeaBuffer;  //<------------ using the pointer to the unmasked events!
    unsigned char* pLeft  = leftOutputImage->getRawImage(); 
    unsigned char* pRight = rightOutputImage->getRawImage();
    unsigned char* pMemL  = leftInputImage->getRawImage();
    int          rowSize  = leftOutputImage->getRowSize();
    int       rowSizeFea  = leftFeaOutputImage->getRowSize();
    unsigned long ts;
    short pol, cam;
    aer* bufferFEA_copy = bufferFEA;
    // visiting a discrete number of events
    int countUnmapped = 0;
    int deviance      = 20;
    int devianceFea   = 20;      
    //#############################################################################        
        for(int i = 0; i < countEvent; i++ ) {
            ts  = iterEvent->ts;
            pol = iterEvent->pol;
            cam = iterEvent->cam;
            //printf("cam %d \n", cam);
            if(cam==1) {              
                if(ts > lastTimestampLeft) {
                    countEventLeft++;
                    if(firstHalf){
                        if(ts < 0x807FFFFF) {
                            lastTimestampLeft = ts;
                        }
                        //else {
                        //    printf("second half timestamp in first %08x %08x \n",ts,lastTimestampLeft  );
                        //}
                    }
                    else {
                        lastTimestampLeft = ts;
                    }
                    
                    
                    //printf(" %d %d %08x \n",iterEvent->x,iterEvent->y, ts );
                    int x = RETINA_SIZE - iterEvent->x - 1 ;
                    int y = iterEvent->y  ;
                    //if((x >127)||(x<0)||(y>127)||(y<0)) break;
                    int posImage     = y * rowSize + x;
                    
                    for (int i = 0; i< 5 ; i++) {                
                        int pos      = lut[i * RETINA_SIZE * RETINA_SIZE +  y * RETINA_SIZE + x ];     
          
                        //printf("        pos ; %d ", pos);
                        if(pos == -1) {
                            if (x % 2 == 1) {
                                //printf("not mapped event %d \n",x + y * RETINA_SIZE);
                                countUnmapped++;
                            }                
                        }
                        else {
                            //creating an event                            
                            int yevent_tmp   = pos / FEATUR_SIZE;
                            int yevent       = FEATUR_SIZE - yevent_tmp;
                            int xevent_tmp   = pos - yevent_tmp * FEATUR_SIZE;
                            int xevent       = xevent_tmp;
                            int polevent     = pol < 0 ? 0 : 1;
                            int cameraevent  = 1;
                            unsigned long blob = 0;
                            int posFeaImage     = yevent * rowSizeFea + xevent ;
                            
                            //if(ts!=0) {
                            //    printf(" %d %d %d ---> ", i ,x, y  );
                            //    printf(" %d %d %d %d %08x %08x \n",pos, yevent,xevent,posFeaImage,blob, ts);
                            //}
                            unmask_events.maskEvent(xevent, yevent, polevent, cameraevent, blob);

                            //blob = 0x00001021;
                            //printf("Given pos %d extracts x=%d and y=%d pol=%d blob=%08x evPU = %08x \n", pos, xevent, yevent, pol, blob, ts);
                            
                            
                            unsigned char* pFeaLeft = leftFeaOutputImage->getRawImage();
                            if(polevent > 0) {
                                if(pFeaLeft[posFeaImage] <= 255 - devianceFea) {
                                    pFeaLeft[posFeaImage] += devianceFea ;
                                }
                                else {
                                    pFeaLeft[posFeaImage] = 255;
                                }
                            }
                            else {
                                if(pFeaLeft[posFeaImage] >= devianceFea) {
                                    pFeaLeft[posFeaImage] -= devianceFea ;
                                }
                                else {
                                    pFeaLeft[posFeaImage] = 0;
                                }
                            }
                            
                            // sending the event if we pass thresholds
                            unsigned long evPU;
                            evPU = 0;
                            evPU = evPU | polevent;
                            evPU = evPU | (xevent << 1);
                            evPU = evPU | (yevent << 8);
                            evPU = evPU | (cameraevent << 15);                            
                            blob = blob & 0x0000FFFF;                            
                            
                            if(pFeaLeft[posFeaImage] > 240) {
                                bufferFEA_copy->address   = (u32) blob;
                                bufferFEA_copy->timestamp = (u32) ts;
                                //#ifdef VERBOSE
                                //fprintf(fdebug,"%08X %08X \n",ts,blob);  
                                //#endif
                                
                                //fprintf(fout,"%08X %08X \n",bufferFEA_copy->address,ts);
                                bufferFEA_copy++; // jumping to the next event(u32,u32)
                                countEventToSend++;
                            }
                            else if(pFeaLeft[posFeaImage] < 10) {
                                bufferFEA_copy->address   = (u32) blob;
                                bufferFEA_copy->timestamp = (u32) ts;
                                //#ifdef VERBOSE
                                //fprintf(fdebug,"%08X %08X \n",ts,blob);  
                                //#endif
                                
                                //fprintf(fout,"%08X %08X \n",bufferFEA_copy->address,ts);
                                bufferFEA_copy++; // jumping to the next event(u32,u32)
                                countEventToSend++;
                            }                           
                                                
                        } //end else
                        
                    } //end for i 5
                    
                    if (outLeftPort.getOutputCount()){
                        
                        int padding    = leftOutputImage->getPadding();
                        int rowsizeOut = leftOutputImage->getRowSize();
                        float lambda   = 1.0;
                        
                        //creating the debug image
                        if(pol > 0){
                            if(pLeft[posImage] <= (255 - deviance)) {
                                //pLeft[pos] = 0.6 * pLeft[pos] + 0.4 * (pLeft[pos] + 40);
                                pLeft[posImage] =  pLeft[posImage] + deviance;
                            }
                            else {
                                pLeft[posImage] = 255;
                            }
                            //pMemL[pos] = (int)((1 - lambda) * (double) pMemL[pos] + lambda * (double) (pMemL[pos] + deviance));
                            //pLeft[posImage] = pMemL[posImage];
                            
                            
                            
                        }
                        if(pol<0){ 
                            //pLeft[pos] = 0.6 * pLeft[pos] + 0.4 * (pLeft[pos] - 40);
                            //pMemL[pos] = (int) ((1 - lambda) * (double) pMemL[pos] + lambda * (double)(pMemL[pos] - deviance));
                            if(pLeft[posImage] >= deviance) {
                                pLeft[posImage] =  pLeft[posImage] - deviance;
                            }
                            else {
                                pLeft[posImage] = 0;
                            }
                            //pLeft[posImage] = pMemL[posImage];
                            
                            //  if(pMemL[pos] < 127 - 70) {
                            //  bufferFEA_copy->address   = (u32) blob;
                            //  bufferFEA_copy->timestamp = (u32) ts;                
                            //  #ifdef VERBOSE
                            //  fprintf(fdebug,"%08X %08X \n",blob,ts);  
                            //  #endif                                
                            //fprintf(fout,"%08X %08X \n",bufferFEA_copy->address,ts);
                            //  bufferFEA_copy++; // jumping to the next event(u32,u32)
                            // countEvent++; 
                            // }
                            
                        }
                        
                        
                    } //end of if outLeftPort
                    
                    
                    
                    //printf("pointing in the lut at position %d %d %d \n",x, y, x + y * RETINA_SIZE);
                    // extra output positions are pointed by i
                
                } // end if ts > lasttimestamp
                
            } //end of if cam!=0
            // -------------------------- CAMERA 0 ----------------------------------
            else if(cam==0) {
                countEventRight++;
                                
                lastTimestampRight = ts;
                //printf(" %d %d %08x \n",iterEvent->x,iterEvent->y, ts );
                int x = RETINA_SIZE - iterEvent->x;
                int y = RETINA_SIZE - iterEvent->y;
                //if((x >127)||(x<0)||(y>127)||(y<0)) break;
                int posImage     = y * rowSize + x;


                for (int i = 0; i< 5 ; i++) {                
                    int pos      = lut[i * RETINA_SIZE * RETINA_SIZE +  y * RETINA_SIZE + x ];                      
                    
                    //printf("        pos ; %d ", pos);
                    if(pos == -1) {
                        if (x % 2 == 1) {
                            //printf("not mapped event %d \n",x + y * RETINA_SIZE);
                            countUnmapped++;
                        }                
                    }
                    else {
                        //creating an event
                        
                        int xevent_tmp   = pos / FEATUR_SIZE;
                        int xevent       = FEATUR_SIZE - xevent_tmp;
                        int yevent_tmp   = pos - xevent_tmp * FEATUR_SIZE;
                        int yevent       = yevent_tmp;
                        int polevent     = pol < 0 ? 0 : 1;
                        int cameraevent  = 1;
                        unsigned long blob = 0;
                        int posFeaImage     = y * rowSizeFea + x;
                        
                        //if(ts!=0) {
                        //    printf(" %d %d %d %08x %08x \n",pos, yevent, xevent,blob, ts);
                        //}
                        unmask_events.maskEvent(xevent, yevent, polevent, cameraevent, blob);
                        //unsigned long evPU;
                        //evPU = 0;
                        //evPU = evPU | polevent;
                        //evPU = evPU | (xevent << 1);
                        //evPU = evPU | (yevent << 8);
                        //evPU = evPU | (cameraevent << 15);
                        
                        //blob = blob & 0x0000FFFF;
                        //blob = 0x00001021;
                        //printf("Given pos %d extracts x=%d and y=%d pol=%d blob=%08x evPU = %08x \n", pos, xevent, yevent, pol, blob, ts);
                        
                        
                        //unsigned char* pFeaRight = rightFeaOutputImage->getRawImage();
                        //pFeaRight[posFeaImage] = 255;
                        
                    } //end else
                    
                } //end for i 5
                 if (outRightPort.getOutputCount()){
                    
                    int padding    = rightOutputImage->getPadding();
                    int rowsizeOut = rightOutputImage->getRowSize();
                    float lambda   = 1.0;
                    //creating the debug image
                    if(pol > 0){
                        //pLeft[pos] = 0.6 * pLeft[pos] + 0.4 * (pLeft[pos] + 40);
                        if(pRight[posImage] <= (255 - deviance)) {
                            pRight[posImage] =  pRight[posImage] + deviance;
                        }
                        else {
                            pRight[posImage] = 255;
                        }
                        //pMemL[pos] = (int)((1 - lambda) * (double) pMemL[pos] + lambda * (double) (pMemL[pos] + deviance));
                        //pLeft[posImage] = pMemL[posImage];
                        
                        //  if(pMemL[pos] > 127 + 70) {
                        //  bufferFEA_copy->address   = (u32) blob;
                        //  bufferFEA_copy->timestamp = (u32) ts;
                        //  #ifdef VERBOSE
                        //  fprintf(fdebug,"%08X %08X \n",blob,ts);  
                        //  #endif
                        
                        //fprintf(fout,"%08X %08X \n",bufferFEA_copy->address,ts);
                        //  bufferFEA_copy++; // jumping to the next event(u32,u32)
                        //  countEvent++;
                             // }
                        
                    }
                    if(pol<0){ 
                        //pLeft[pos] = 0.6 * pLeft[pos] + 0.4 * (pLeft[pos] - 40);
                        //pMemL[pos] = (int) ((1 - lambda) * (double) pMemL[pos] + lambda * (double)(pMemL[pos] - deviance));
                        if(pRight[posImage] >= deviance) {
                            pRight[posImage] =  pRight[posImage] - deviance;
                        }
                        else {
                            pRight[posImage] = 0;
                        }
                        //pLeft[posImage] = pMemL[posImage];
                        
                        //  if(pMemL[pos] < 127 - 70) {
                        //  bufferFEA_copy->address   = (u32) blob;
                        //  bufferFEA_copy->timestamp = (u32) ts;                
                        //  #ifdef VERBOSE
                        //  fprintf(fdebug,"%08X %08X \n",blob,ts);  
                        //  #endif                                
                        //fprintf(fout,"%08X %08X \n",bufferFEA_copy->address,ts);
                        //  bufferFEA_copy++; // jumping to the next event(u32,u32)
                        // countEvent++; 
                        // }
                        
                    }                                        
                } //end of if outLeftPort                
            }
            else {
                printf("error in the camera value %d \n", cam);
            }

                            
            //printf("\n");
            iterEvent++;
        } //end for i
        //############################################################################
}

void efExtractorThread::run() {   
    count++;
    countEvent = 0;

    //printf("counter %d \n", count);
    bool flagCopy;
    if(!idle) {
        
        //ImageOf<PixelMono> &left = outLeftPort.prepare();  
        //left.resize(128, 128);
        //left.zero();
        unsigned char* pLeft  = leftOutputImage->getRawImage(); 
        unsigned char* pRight = rightOutputImage->getRawImage();
        unsigned char* pMemL  = leftInputImage->getRawImage();
        //left.zero();
        

        
        // reads the buffer received
        // saves it into a working buffer        
        //printf("returned 0x%x 0x%x \n", bufferCopy, flagCopy);
        if(!bottleHandler) {
            cfConverter->copyChunk(bufferCopy);//memcpy(bufferCopy, bufferRead, 8192);
        }
        else {
             ebHandler->extractBottle(receivedBottle);      
        }
        
        int num_events = CHUNKSIZE >> 3 ;
        uint32_t* buf2 = (uint32_t*)bufferCopy;
        //plotting out
        int countEvent = 0;
        uint32_t* bufCopy2 = (uint32_t*)bufferCopy2;
        firstHalf = true;
        if(buf2[0] < 0x8000F000) {
            firstHalf = true;
        }
        else {
            firstHalf = false;
        }

        for (int evt = 0; evt < num_events; evt++) {
            unsigned long t      = buf2[2 * evt];
            unsigned long blob   = buf2[2 * evt + 1];
            
            if(t > 0x80000000) {
                if(t == 0x88000000 ){
                    lastTimestampLeft = 0;
                    printf("wrap around detected %lu \n", lastTimestampLeft );
                    if(VERBOSE){
                        fprintf(fdebug,"detected_wrap_around \n");
                    }
                    firstHalf = true;
                }
                else {                    
                    if((firstHalf) && (t < 0x8000F000)) {
                        if(t > lastTimestampLeft)  {
                            lastTimestampLeft = t;
                            *bufCopy2 = buf2[2 *evt]; bufCopy2++;
                            *bufCopy2 = buf2[2 * evt + 1]; bufCopy2++;
                            countEvent++;
                            if(VERBOSE) {
                                fprintf(fdebug,"%08X %08X \n",(unsigned int) t,(unsigned int) blob);        
                            }
                        }
                    }
                    else if((t >= 0x8000F000)&&(t<80010000)) {
                        printf("first half  = false\n");
                        firstHalf = false;
                    }
                    else if((!firstHalf) && (t>0x8000F000)) {
                        if(t > lastTimestampLeft)  {
                            lastTimestampLeft = t;
                            *bufCopy2 = buf2[2 *evt]; bufCopy2++;
                            *bufCopy2 = buf2[2 * evt + 1]; bufCopy2++;
                            countEvent++;
                            if(VERBOSE) {
                                fprintf(fdebug,"%08X %08X \n",(unsigned int) t,(unsigned int) blob);        
                            }
                        }
                    }
                    else if((t < 0x8000F000)&&(!firstHalf)) {
                        printf("undetected wrap-around \n");
                        if(VERBOSE) {
                            fprintf(fdebug,"undetected_wrap_around \n");
                            fprintf(fdebug,"%08X %08X \n",(unsigned int) t,(unsigned int) blob);   
                            printf("undetected wrap aroung");
                        }
                        firstHalf = true;
                    }                        
                }
            }            
        }
        
        
        //int unreadDim = selectUnreadBuffer(bufferCopy, flagCopy, resultCopy);
        //printf("Unmasking events:  %d \n", unreadDim);

        // extract a chunk/unmask the chunk       
        int dim = unmask_events.unmaskData(bufferCopy2,countEvent * sizeof(uint32_t),eventFeaBuffer);        
        //printf("event converted %d whereas %d \n", dim, countEvent);          

        //printf("         countLeft %d \n" , countEventLeft);
        //printf("         countRight %d \n", countEventRight);

        //generating the memory
        int countEventToSend = 0;
        generateMemory(countEvent, countEventToSend);

        
        // writing images out on ports 
        outLeftPort.prepare()     = *leftOutputImage;
        outLeftPort.write();
        outRightPort.prepare()    = *rightOutputImage;
        outRightPort.write();
        outFeaLeftPort.prepare()  = *leftFeaOutputImage;
        outFeaLeftPort.write();
        outFeaRightPort.prepare() = *rightFeaOutputImage;
        outFeaRightPort.write();
        
        // leaking section of the algorithm (retina space)
        pMemL  = leftInputImage->getRawImage();
        pLeft  = leftOutputImage->getRawImage();
        int padding = leftOutputImage->getPadding();
        for(int row =0; row < RETINA_SIZE; row++) {
            for (int col = 0; col< RETINA_SIZE; col++) {
                if(*pLeft >= 127 + CONST_DECREMENT) {                
                    *pLeft -= CONST_DECREMENT;
                }
                else if(*pLeft <= 127 - CONST_DECREMENT) {
                    *pLeft += CONST_DECREMENT;
                }
                else{
                    *pLeft = 127;
                }
                if(*pRight >= 127 + CONST_DECREMENT) {                
                    *pRight -= CONST_DECREMENT;
                }
                else if(*pRight <= 127 - CONST_DECREMENT) {
                    *pRight += CONST_DECREMENT;
                }
                else{
                    *pRight = 127;
                }
                
                pLeft++;
                pRight++;
                //*pLeft = *pMemL;
                //pLeft++;
            }
            //pMemL += padding;                
            pLeft += padding;
            pRight+= padding;
        }
        
        
        // leaking section of the algorithm ( feature map)
        unsigned char* pFeaLeft = leftFeaOutputImage->getRawImage();
        
        padding  = leftFeaOutputImage->getPadding();
        for(int row =0; row < FEATUR_SIZE; row++) {
            for (int col = 0; col< FEATUR_SIZE; col++) {
                if(*pFeaLeft >= 127 + CONST_DECREMENT) {                
                    *pFeaLeft -= CONST_DECREMENT;
                }
                else if(*pFeaLeft <= 127 - CONST_DECREMENT) {
                    *pFeaLeft += CONST_DECREMENT;
                }
                else{
                    *pFeaLeft = 127;
                }
                
                //if(*pFeaRight >= 127 + CONST_DECREMENT) {                
                //    *pFeaRight -= CONST_DECREMENT;
                //}
                //else if(*pFeaRight <= 127 - CONST_DECREMENT) {
                //    *pFeaRight += CONST_DECREMENT;
                //}
                //else{
                //    *pFeaRight = 127;
                //}                
                
                pFeaLeft++;
                //pFeaRight++;
                
            }
                            
            pFeaLeft += padding;
            //pFeaRight+= padding;
        }
        
        //building feature events
        char* buffer = (char*) bufferFEA;
        aer* bFea_copy = bufferFEA;
        //int sz = countEvent * sizeof(aer);
        int sz = countEventToSend * sizeof(aer);
        // sending events 
        if ((outEventPort.getOutputCount()) && (countEventToSend > 0)) {     
            if (VERBOSE) {
                for (int i = 0; i < countEventToSend; i++) {
                    u32 blob      = bFea_copy[i].address;
                    u32 timestamp = bFea_copy[i].timestamp;
                    fprintf(fout,">%08x %08x \n",timestamp, blob);
                }
                fprintf(fout, ">>>>>>>>>>>>>>>>>>>>>>>>>>> \n");
            }
            //printf("Sending \n");
            eventBuffer data2send(buffer,sz);    
            eventBuffer& tmp = outEventPort.prepare();
            tmp = data2send;
            outEventPort.write();
        }        
    } //end of idle
}



/*
#ifdef COMMENT                
                  for (int i = 0; i< 5 ; i++) {                
                  int pos      = lut[i * RETINA_SIZE * RETINA_SIZE +  y * RETINA_SIZE + x ];                        
                  
                  //printf("        pos ; %d ", pos);
                  if(pos == -1) {
                  if (x % 2 == 1) {
                  //printf("not mapped event %d \n",x + y * RETINA_SIZE);
                  countUnmapped++;
                  }                
                  }
                  else {
                  //creating an event
                  
                  int xevent_tmp   = pos / FEATUR_SIZE;
                  int xevent       = FEATUR_SIZE - xevent_tmp;
                  int yevent_tmp   = pos - xevent_tmp * FEATUR_SIZE;
                  int yevent       = yevent_tmp;
                  int polevent     = pol < 0 ? 0 : 1;
                  int cameraevent  = 0;
                  unsigned long blob = 0;
                  int posImage     = y * rowSize + x;
                  
                  //if(ts!=0) {
                  //    printf(" %d %d %d %08x %08x \n",pos, yevent, xevent,blob, ts);
                  //}
                  unmask_events.maskEvent(xevent, yevent, polevent, cameraevent, blob);
                  //unsigned long evPU;
                  //evPU = 0;
                  //evPU = evPU | polevent;
                  //evPU = evPU | (xevent << 1);
                  //evPU = evPU | (yevent << 8);
                  //evPU = evPU | (cameraevent << 15);
                  
                  //blob = blob & 0x0000FFFF;
                  //blob = 0x00001021;
                  //printf("Given pos %d extracts x=%d and y=%d pol=%d blob=%08x evPU = %08x \n", pos, xevent, yevent, pol, blob, ts);
                  
                  //
                  //bufferFEA_copy->address   = (u32) blob;
                  //bufferFEA_copy->timestamp = (u32) ts;                
                  ////fprintf(fout,"%08X %08X \n",bufferFEA_copy->address,ts);
                  //bufferFEA_copy++; // jumping to the next event(u32,u32)
                  //countEvent++;
                  //
                  
                  //pLeft = pointer to the left image
                  //pMem  = pointer to the left input image
                  
                  
                  } //end else
                  
                  } //end for i 5
#ifdef
*/



