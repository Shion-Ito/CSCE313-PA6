#include "common.h"
#include "BoundedBuffer.h"
#include "Histogram.h"
#include "common.h"
#include "HistogramCollection.h"
#include "TCPreqchannel.h"
#include <sys/epoll.h>
#include <thread>
using namespace std;



void patient_thread_function(int n,int pno, BoundedBuffer* request_buffer){
    datamsg d(pno,0.0,1);
    double resp = 0;
    for(int i = 0; i<n; i++){
        request_buffer->push((char*)&d,sizeof(datamsg));
        d.seconds+= 0.004;
    }

}

void file_thread_function (string fname, BoundedBuffer* request_buffer,TCPRequestChannel *chan, int mb){
    //1. create the file
    string recvfname = "recv/" + fname;
    //make it as long as the original length
    char buf[1024];
    filemsg f(0,0);
    memcpy(buf,&f,sizeof(f));
    strcpy(buf+sizeof(f),fname.c_str());
    chan->cwrite (buf,sizeof(f)+fname.size()+1);
    __int64_t filelength;
    chan->cread (&filelength, sizeof(filelength));

    FILE* fp = fopen(recvfname.c_str(), "w");
    fseek (fp, filelength, SEEK_SET);
    fclose(fp);

    //2. generate all the file messages
    filemsg* fm = (filemsg *) buf;
    __int64_t remlen = filelength;

    while(remlen > 0){
        fm-> length = min(remlen, (__int64_t) mb);
        request_buffer->push(buf, sizeof(filemsg) + fname.size() +1);
        fm->offset += fm-> length;
        remlen -= fm->length;

    }


}

void setnonblocking(int fd){
    int flags;
    if(-1 == (flags = fcntl(fd,F_GETFL,0)))
        flags = 0;
    fcntl(fd,F_SETFL,flags | O_NONBLOCK);
}

void event_polling_thread(int w, int mb , TCPRequestChannel* wchans[] ,BoundedBuffer* request_buffer, HistogramCollection* hc){    
    printf("%s \n","inside evp");
    char buf[mb];
    double resp = 0;

    char recvbuf[mb];

    struct epoll_event ev;
    struct epoll_event events[w];

    // create an empty epoll list
    int epollfd = epoll_create1 (0);
    if(epollfd == -1){
        EXITONERROR("epoll_create1");
    }

    unordered_map<int,int> fd_to_index;
    vector<vector<char>> state(w);
    bool quit_recv = false;

    //priming + adding each rfd to the list
    int nsent = 0, nrecv = 0; 
    for(int i = 0; i< w; i++){
        int sz = request_buffer->pop (buf,1024);
        if(*(MESSAGE_TYPE*) buf == QUIT_MSG){
            quit_recv = true;
            break;
        }
        wchans[i]->cwrite (buf,sz);
        nsent++;
        state[i] = vector<char>(buf,buf+sz); //record the state i

        int rfd = wchans[i] ->getfd();

        // fcntl(rfd,F_SETFL, O_NONBLOCK);
        
        fd_to_index[rfd] = i;

        setnonblocking(rfd); //INSERTED!!!!! UNKNOWN 

        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = rfd;

        if (epoll_ctl(epollfd,EPOLL_CTL_ADD,rfd,&ev) == -1){
            EXITONERROR("epoll_ctl: listen_sock");
        }
    }

    // nsent = w, nrecvd = 0;

    while(true){
        if(quit_recv && nrecv == nsent)
            break;
        int nfds = epoll_wait(epollfd,events,w,-1);
        if(nfds == -1){
            EXITONERROR ("epoll_wait");
        }
        for(int i = 0; i <nfds; i++){
            int rfd = events[i].data.fd;
            int index = fd_to_index[rfd];

            int resp = wchans[index]->cread(recvbuf,mb);
            nrecv++;
            //process (recvbuf)
            vector<char> req = state[index];
            char* request = req.data();
            MESSAGE_TYPE*m = (MESSAGE_TYPE *) request;

            if(*m == DATA_MSG){
                hc-> update(((datamsg *)request)-> person, *(double*)recvbuf);
            }else if(*m == FILE_MSG){

                filemsg* fm = (filemsg*) request;
                string fname = (char*)(fm + 1);
                int sz = sizeof(filemsg) + fname.size() + 1;
                //wchans[i]->cwrite (request, sz);
                //wchans[i]->cread (recvbuf, mb);
            
                string recvfname = "recv/" +fname;
                FILE* fp = fopen(recvfname.c_str(), "r+");
                fseek(fp,fm->offset , SEEK_SET);
                fwrite(recvbuf,1,fm->length,fp);
                fclose(fp);
            }


            // reuse
            if(!quit_recv){
                int req_sz = request_buffer->pop (buf,sizeof(buf));
                if(*(MESSAGE_TYPE*) buf == QUIT_MSG){
                    quit_recv = true;
                }else{
                    wchans[index]->cwrite (buf,req_sz);
                    state[index] = vector<char> (buf,buf+req_sz);
                    nsent++;
                }
            }

        }
    }
}



int main(int argc, char *argv[])
{
    int n = 15000;    //default number of requests per "patient"
    int p = 10;     // number of patients [1,15]
    int w = 100;    //default number of worker threads
    int b = 20; 	// default capacity of the request buffer, you should change this default
	int m = MAX_MESSAGE; 	// default capacity of the message buffer
    char* m_size = "";

    bool fileTime = false;
    srand(time_t(NULL));
    string fname = "10.csv";
    int opt = -1;
    string host, port;
    while ((opt = getopt (argc, argv, "m:n:b:w:p:f:h:r:")) != -1){
        switch (opt) {
            case 'm':
                m_size = optarg;
                m = atoi (optarg);
                break;
            case 'n':
                n = atoi(optarg);
                break;
            case 'p':
                p = atoi (optarg);
                break;
            case 'b':
                b = atoi(optarg);
                break;
            case 'w':
                w = atoi(optarg);
                break;
            case 'f':
                fname = optarg;
                fileTime = true;
                break;
            case 'h':
                host = optarg;
                break;
            case 'r':
                port = optarg;
                break;
        }
    }

    
    BoundedBuffer request_buffer(b);
	HistogramCollection hc;
	
    //making histograms and adding to the histogram collection hc
    for(int i = 0; i< p; i++){
        Histogram* h = new Histogram(10, -2.0, 2.0);
        hc.add(h);
    }

    // make w worker channels
    TCPRequestChannel** wchans = new TCPRequestChannel*[w];
    for(int i = 0; i< w ; i++){
        wchans[i] = new TCPRequestChannel(host,port);
    }
	
	
    struct timeval start, end;
    gettimeofday (&start, 0);

    if(!fileTime){
        cout << "Obtaining data points for histogram"<< endl;
        /* Start all threads here */
        thread patient[p];
        for (int i = 0; i < p ; i++){
            patient[i] = thread (patient_thread_function,n,i+1,&request_buffer);
        }

        thread evp(event_polling_thread,w,m, wchans,&request_buffer,&hc);

        // join patient thread
        for(int i = 0;i<p;i++){
            patient[i].join();
        }
        cout << "Patient threads finsihed" << endl;
        //Only one Quit message
        MESSAGE_TYPE q = QUIT_MSG;
        request_buffer.push((char*) &q, sizeof(q));  
        evp.join();
        cout << "Worker threads finished" << endl;

        for(int i = 0; i< w; i++){
            wchans[i]->cwrite (&q,sizeof(MESSAGE_TYPE));
            delete wchans[i];
        }
        delete[] wchans;


        gettimeofday (&end, 0);
        // print the results
        hc.print ();
        int secs = (end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)/(int) 1e6;
        int usecs = (int)(end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)%((int) 1e6);
        cout << "Took " << secs << " seconds and " << usecs << " micro seconds" << endl;

        cout << "All Done!!!" << endl;
  
  

    }else{

        TCPRequestChannel * chan = new TCPRequestChannel(host,port);
        thread filethread(file_thread_function,fname,&request_buffer, chan, m);
        thread evp(event_polling_thread,w,m, wchans,&request_buffer,&hc);

        filethread.join();
        cout << "Patient threads/filethread finished"<< endl;

        MESSAGE_TYPE q = QUIT_MSG;
        request_buffer.push((char*) &q, sizeof(q));  
        evp.join();
        cout << "Worker threads finished" << endl;

        for(int i = 0; i< w; i++){
            wchans[i]->cwrite (&q,sizeof(MESSAGE_TYPE));
            delete wchans[i];
        }
        delete[] wchans;

        delete chan;

        gettimeofday (&end, 0);
        // print the results
        int secs = (end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)/(int) 1e6;
        int usecs = (int)(end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)%((int) 1e6);
        cout << "Took " << secs << " seconds and " << usecs << " micro seconds" << endl;


        cout << "All Done!!!" << endl;
 
        
    }




    
}
