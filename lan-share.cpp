#include "sys/socket.h"
#include "sys/types.h"
#include "arpa/inet.h"
#include "netdb.h"
#include "string.h"
#include "iostream"
#include "memory"
#include "vector"
#include "unistd.h"
#include "thread"
#include "mutex"
#include "random"
#include "fstream"
#include "iomanip"
#include "sstream"
#include "limits"
#include "cstdio"
#include "cerrno"
#include "queue.h"

using namespace std;


//broadCast port is 9999
//broadCast addr is 255.255.255.255
const int addrlen=17;
const char DiscoverAddr[addrlen]="255.255.255.255\0";
const unsigned int port =9999;

const int headerlen=6;
const int namelen=25;
const int idLen=16;
const int FileNameLen=256;
const size_t chunkSize=64*1024; //64KB per send/recv during the file transfer


const int sockFail=-2;
const int bindFail=-3;
const int sockNameFail=-4;
const int ptonFail=-5;
const int listenFail=-6;

bool debugMode=false; //set once in main from argv before any threads start

mutex listMut;
mutex offerMut; //guards incomingOffers
mutex outMut;   //guards outgoingOffers

#pragma pack(push,1)
struct broadcast
{
    char header[headerlen];
    char MyName[namelen];
    uint8_t myid[idLen]; //random 16 digit id
    uint16_t myTcp; //network protocol
};
#pragma pack(pop)

#pragma pack(push,1)
struct TCP_offer
{
    char header[headerlen];
    char SenderName[namelen];
    uint8_t fileName[FileNameLen];
    uint64_t fileSize;
};

struct reply
{
    char header[headerlen];
    uint8_t Reply;// 1 == yes or 0 == no
};
#pragma pack(pop)

struct pendingOffer //an offer someone sent us that we havent said yes or no to yet
{
    int sock; //the accepted connection the sender is sitting on the other end waiting for our reply
    TCP_offer offer;
};

Queue<pendingOffer> incomingOffers; //first come first served protected by offerMut

struct outgoingOffer //an offer we sent that the other side hasnt answered yet
{
    int id;
    int sock;
    char peerName[namelen];
    char fileName[FileNameLen];
};

vector<outgoingOffer> outgoingOffers; //vector not a queue cuz cancel can pull any one out protected by outMut
int nextOfferId=1;                    //protected by outMut

bool checkValidArgCount(int c)
{
    return c<2;
}

void clearInput() //clear() only resets the flags ignore() throws away the junk still sitting in the buffer
{
    cin.clear();
    cin.ignore(numeric_limits<streamsize>::max(),'\n');
}

void fillBroadCastStruct(broadcast& MyBroadCastMessage,char* name,unsigned short myPort)
{
    char header[headerlen]="LSH1\0";

    int size=strlen(name);
    size=min(size,namelen-1);
    memset(&MyBroadCastMessage,0,sizeof(broadcast));
    memcpy(MyBroadCastMessage.MyName,name,size);
    strcpy(MyBroadCastMessage.header,header);
    MyBroadCastMessage.myTcp=myPort;
    random_device rd;
    for (int i = 0; i <idLen; i++) MyBroadCastMessage.myid[i] = rd() & 0xFF;//keep only the lowest 8 bits

}

void setUpAddrStorage(sockaddr_storage& Myaddr)
{
    memset(&Myaddr,0,sizeof(Myaddr));
    Myaddr.ss_family=AF_INET;
    struct sockaddr_in * MyIpv4Addr=(sockaddr_in * )&Myaddr;
    MyIpv4Addr->sin_port=0; //host to network doesnt matter here cuz 0 is represented the same in both endains
    MyIpv4Addr->sin_addr.s_addr=htonl(INADDR_ANY);
}


void displayOwnInfo(broadcast toDisplay)
{
    cout<<"My name is "<<toDisplay.MyName<<"\n"
        <<"My port is "<<ntohs(toDisplay.myTcp)<<"\n"
        <<"My header is "<<toDisplay.header<<"\n";
    cout<<"My id is ";
    for(int i=0;i<idLen;i++) cout<<(int)toDisplay.myid[i];
    cout<<"\n";
}

struct peer //need to add time stamps so i can remove dead peers
{
    sockaddr_storage PeerAddr;
    char PeerName[namelen];
    uint8_t PeerId[idLen]; //random 16 digit id
    uint16_t TcpPort; //network protocol
};

void copyPeer(peer& P, peer* heapP)
{
    memcpy(&P.PeerAddr, &heapP->PeerAddr, sizeof(sockaddr_storage));
    memcpy(P.PeerName, heapP->PeerName, sizeof(P.PeerName));
    memcpy(P.PeerId, heapP->PeerId, sizeof(P.PeerId));
    memcpy(&P.TcpPort, &heapP->TcpPort, sizeof(P.TcpPort));
}

bool setDebug(int c, char* argv[])
{
    if(c>2)
    {
        if(!strcmp(argv[2],"yes\0")) return true;
    }
    return false;
}

void DisplayHeader()
{

    //raw string literal so the box drawing chars and spacing print exactly as written no escaping needed
    cout<<R"(
╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸
╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸
         ╻ ╻┏━╸╻  ┏━╸┏━┓┏┳┓┏━╸   ╺┳╸┏━┓   ╻  ┏━┓┏┓╻   ┏━┓╻ ╻┏━┓┏━┓┏━╸
         ┃╻┃┣╸ ┃  ┃  ┃ ┃┃┃┃┣╸     ┃ ┃ ┃   ┃  ┣━┫┃┗┫╺━╸┗━┓┣━┫┣━┫┣┳┛┣╸
         ┗┻┛┗━╸┗━╸┗━╸┗━┛╹ ╹┗━╸    ╹ ┗━┛   ┗━╸╹ ╹╹ ╹   ┗━┛╹ ╹╹ ╹╹┗╸┗━╸
                  ┏┳┓┏━┓╺┳┓┏━╸   ┏┓ ╻ ╻   ╻ ╻┏━┓┏┳┓┏┳┓┏━┓╺┳┓   ┏━┓┏━╸╻ ╻┏━┓┏┓╻
                  ┃┃┃┣━┫ ┃┃┣╸    ┣┻┓┗┳┛   ┣━┫┣━┫┃┃┃┃┃┃┣━┫ ┃┃   ┣┳┛┣╸ ┣━┫┣━┫┃┗┫
                  ╹ ╹╹ ╹╺┻┛┗━╸   ┗━┛ ╹    ╹ ╹╹ ╹╹ ╹╹ ╹╹ ╹╺┻┛   ╹┗╸┗━╸╹ ╹╹ ╹╹ ╹

╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸
╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸╺━╸
)";

}

string humanSize(uint64_t bytes) //5000000 -> 4.8 MB
{
    const char* units[]={"B","KB","MB","GB","TB"};
    double size=bytes;
    int u=0;
    while(size>=1024 && u<4){size/=1024; u++;}
    ostringstream out;
    if(u==0) out<<bytes<<" B";
    else out<<fixed<<setprecision(1)<<size<<" "<<units[u];
    return out.str();
}

void printPeer(const peer& p,size_t idx)
{
    char ip[INET_ADDRSTRLEN]="";
    if(idx==0) strcpy(ip,"this machine"); //we never got our own broadcast so theres no address stored for us
    else inet_ntop(AF_INET,&((const sockaddr_in*)&p.PeerAddr)->sin_addr,ip,sizeof(ip));

    string name=p.PeerName;
    if(idx==0) name+=" (me)";

    cout<<"  "<<left<<setw(4)<<idx<<setw(namelen+6)<<name<<setw(17)<<ip<<ntohs(p.TcpPort);
    if(debugMode)
    {
        cout<<"   ";
        for(int i=0;i<idLen;i++) cout<<hex<<setw(2)<<setfill('0')<<right<<(int)p.PeerId[i];
        cout<<dec<<setfill(' ');
    }
    cout<<"\n";
}

void PrintPeers(const vector<peer>& list)
{
    cout<<"\n  "<<left<<setw(4)<<"#"<<setw(namelen+6)<<"Name"<<setw(17)<<"IP"<<"Port";
    if(debugMode) cout<<"    Id";
    cout<<"\n  "<<string(debugMode ? 89 : 56,'-')<<"\n";

    for(size_t i=0;i<list.size();i++) printPeer(list[i],i);

    if(list.size()==1) cout<<"\n  No other peers found yet\n";
}


void Broadcast(int broadCastSocket,sockaddr_storage BroadcastAddr,broadcast broadcastMessage)
{
    static size_t size= sizeof(sockaddr_in);

    sendto(broadCastSocket,&broadcastMessage,sizeof(broadcast),0,(sockaddr *)&BroadcastAddr,size);
}


bool Discover(int broadCastSocket,sockaddr_storage BroadcastAddr,broadcast broadcastMessage,vector<peer>& list) //really 2 functions disguised as one
{
    struct sockaddr_storage sender;
    memset(&sender,0,sizeof(sender));
    struct sockaddr_in * senderIpv4=(sockaddr_in*)&sender;
    socklen_t size=sizeof(sender);

    struct broadcast MessageRecv;
    memset(&MessageRecv,0,sizeof(broadcast));
    size_t n=recvfrom(broadCastSocket,&MessageRecv,sizeof(broadcast),0,(sockaddr*)senderIpv4,&size);

    if (n != (ssize_t)sizeof(broadcast)) return false;
    if (memcmp(MessageRecv.header, "LSH1", 4) != 0) return false;   // not our protocol
    MessageRecv.MyName[namelen-1] = '\0';

    //now compare peers
    lock_guard<mutex> listlock(listMut);
    for(auto iterator=list.begin();iterator!=list.end();iterator++)
    {
        bool dupe=true;
        for(int j=0;j<idLen;j++){
            if((*iterator).PeerId[j]!=MessageRecv.myid[j]) {dupe=false; break;}
        }

        if(dupe) return false;
    }

    //not a duplicate
    //we found a new peer
    //add it to the list

    int sizeName=strlen(MessageRecv.MyName);
    sizeName=min(sizeName,namelen-1);

    struct peer *p = new peer;




    memset(p,0,sizeof(peer));
    memcpy(p->PeerId,MessageRecv.myid,sizeof(p->PeerId));
    memcpy(p->PeerName,MessageRecv.MyName,sizeName);
    memcpy(&p->PeerAddr,&sender,sizeof(sender));

    p->TcpPort=MessageRecv.myTcp;
    list.push_back(*p);

    delete p; //the vector itself now owns its own version of peer so we need to delete the heap version to avoid leaking memory

    return true;

}


//=======================================================
//          TCP helpers
//                        start
//=======================================================
bool sendAll(int sock,const void* buf,size_t len) //send() can send less than we asked for so keep going till its all out
{
    const char* p=(const char*)buf;
    while(len>0)
    {
        ssize_t n=send(sock,p,len,MSG_NOSIGNAL); //MSG_NOSIGNAL so a dead peer doesnt kill us with SIGPIPE
        if(n<=0){perror("send failed"); return false;}
        p+=n;
        len-=n;
    }
    return true;
}

bool recvAll(int sock,void* buf,size_t len,bool quiet=false) //same deal as sendAll recv() can hand us a partial struct
{
    char* p=(char*)buf;
    while(len>0)
    {
        ssize_t n=recv(sock,p,len,0);
        if(n==0){if(!quiet) cerr<<"peer closed the connection\n"; return false;}
        if(n<0){if(!quiet) perror("recv failed"); return false;}
        p+=n;
        len-=n;
    }
    return true;
}

int connectToPeer(const peer& p)
{
    sockaddr_in addr;
    memcpy(&addr,&p.PeerAddr,sizeof(addr)); //ip comes from where their broadcast came from
    addr.sin_family=AF_INET;
    addr.sin_port=p.TcpPort; //swap 9999 for their tcp port already in network order so no htons

    int sock=socket(AF_INET,SOCK_STREAM,0);
    if(sock==-1){perror("socket() call failed for offer socket"); return -1;}

    if(connect(sock,(sockaddr*)&addr,sizeof(addr))==-1){perror("connect failed"); close(sock); return -1;}
    return sock;
}

void printProgress(uint64_t done,uint64_t total)
{
    int percent= total==0 ? 100 : (int)(done*100/total);
    cout<<"\r"<<percent<<"% ("<<done<<"/"<<total<<" bytes)"<<flush;
}
//=======================================================
//          TCP helpers
//                        end
//=======================================================


bool FormTCP_offer(char* myName,TCP_offer*& ret,string& Path);



void printOffer(const TCP_offer* offer)
{
    if (!offer) { cout << "offer is null\n"; return; }

    const char* fname = (const char*)offer->fileName;   // uint8_t[] -> char* for printing

    cout << "---- TCP offer ----\n";
    cout << "Header: " << string(offer->header, strnlen(offer->header, headerlen)) << "\n";
    cout << "Name:   " << string(offer->SenderName, strnlen(offer->SenderName, namelen)) << "\n";
    cout << "File:   " << string(fname, strnlen(fname, FileNameLen)) << "\n";
    cout << "Size:   " << be64toh(offer->fileSize) << " bytes\n";
    cout << "-------------------\n";
}

bool sendFile(int sock,const string& Path,uint64_t fileSize) //runs in a background thread so no progress bar it would scribble over the menu
{
    ifstream File(Path,ios::binary);
    if(!File.is_open()){perror("File failed to open for sending"); return false;}

    vector<char> buf(chunkSize);
    uint64_t sent=0;
    while(sent<fileSize)
    {
        size_t toRead=min((uint64_t)chunkSize,fileSize-sent);
        File.read(buf.data(),toRead);
        if((size_t)File.gcount()!=toRead){cerr<<"\nFile read failed\n"; return false;}
        if(!sendAll(sock,buf.data(),toRead)) return false;
        sent+=toRead;
    }
    return true;
}

bool takeOutgoing(int id) //true if we took it off the list false if cancel already did
{
    lock_guard<mutex> outLock(outMut);
    for(auto iterator=outgoingOffers.begin();iterator!=outgoingOffers.end();iterator++)
    {
        if((*iterator).id==id){outgoingOffers.erase(iterator); return true;}
    }
    return false;
}

//one of these runs per sent offer so the menu is free while the other side makes up their mind
void waitForReply(int id,int sock,string Path,uint64_t fileSize,string peerName,string fileName)
{
    struct reply theirReply;
    memset(&theirReply,0,sizeof(theirReply));
    bool gotReply=recvAll(sock,&theirReply,sizeof(reply),true); //quiet a cancel shows up here as a failed recv

    if(!takeOutgoing(id)) //cancel got to it first CancelOffer already told the user
    {close(sock); return;}

    if(!gotReply){cout<<"\n>> "<<peerName<<" went away before answering your offer for "<<fileName<<"\n"; close(sock); return;}
    if(memcmp(theirReply.header,"Reply",5)!=0){cerr<<"\n>> Got a bad reply from "<<peerName<<"\n"; close(sock); return;}
    if(theirReply.Reply!=1){cout<<"\n>> "<<peerName<<" declined "<<fileName<<"\n"; close(sock); return;}

    cout<<"\n>> "<<peerName<<" accepted "<<fileName<<", sending...\n";
    bool ok=sendFile(sock,Path,fileSize);
    close(sock);
    if(ok) cout<<"\n>> Finished sending "<<fileName<<" to "<<peerName<<"\n";
    else cout<<"\n>> Sending "<<fileName<<" to "<<peerName<<" failed\n";
}

bool CancelOffer()
{
    vector<int> ids;
    {
        lock_guard<mutex> outLock(outMut);
        if(outgoingOffers.empty()){cout<<"You have no offers waiting on an answer\n"; return false;}
        for(size_t i=0;i<outgoingOffers.size();i++)
        {
            cout<<i+1<<": "<<outgoingOffers[i].fileName<<" to "<<outgoingOffers[i].peerName<<"\n";
            ids.push_back(outgoingOffers[i].id); //remember ids the list can change while the user types
        }
    }

    int pick=-1;
    while(true)
    {
        cout<<"Enter the offer to cancel (0 to go back): ";
        if(cin>>pick && pick>=0 && pick<=(int)ids.size()) break;
        if(cin.eof()) return false;
        cout<<"Not a valid offer\n";
        clearInput();
    }
    if(pick==0) return false;

    lock_guard<mutex> outLock(outMut);
    for(auto iterator=outgoingOffers.begin();iterator!=outgoingOffers.end();iterator++)
    {
        if((*iterator).id!=ids[pick-1]) continue;
        cout<<"Cancelled "<<(*iterator).fileName<<" to "<<(*iterator).peerName<<"\n";
        shutdown((*iterator).sock,SHUT_RDWR); //wakes up waitForReply and the other side sees the connection close and the thread is the one that closes the socket
        outgoingOffers.erase(iterator);
        return true;
    }
    cout<<"Too late, they already answered\n";
    return false;
}

bool FileTransfer(vector<peer>& list) //list[0] is guranteed to have my own self in it
{
    int idx=-1;
    while(true)
    {
        cout<<"\nEnter the Index for the peer you wish to send the file to: ";
        if(cin>>idx)
        {
            lock_guard<mutex> listlock(listMut);
            if(idx>0 && idx<(int)list.size()) break;
        }
        else if(cin.eof()) return false;
        cout<<"\nThe index doesn't belong to a valid peer\n";
        clearInput();
    }

    peer target;
    char myName[namelen];
    {
        lock_guard<mutex> listlock(listMut); //only hold the lock while copying so discover isnt frozen while we type
        target=list[idx];
        memcpy(myName,list[0].PeerName,namelen);
    }

    struct TCP_offer* myOffer=nullptr;
    string Path;
    if(!FormTCP_offer(myName,myOffer,Path)) return false;
    if(debugMode) printOffer(myOffer);

    int offerSock=connectToPeer(target);
    if(offerSock==-1){delete myOffer; return false;}

    bool ok=sendAll(offerSock,myOffer,sizeof(TCP_offer));
    uint64_t fileSize=be64toh(myOffer->fileSize);
    string fileName((const char*)myOffer->fileName);
    delete myOffer; //only need the size and name from here on
    if(!ok){close(offerSock); return false;}

    outgoingOffer out;
    memset(&out,0,sizeof(out));
    out.sock=offerSock;
    memcpy(out.peerName,target.PeerName,namelen);
    memcpy(out.fileName,fileName.c_str(),min(fileName.length(),(size_t)FileNameLen-1));
    {
        lock_guard<mutex> outLock(outMut);
        out.id=nextOfferId++;
        outgoingOffers.push_back(out);
    }

    thread(waitForReply,out.id,offerSock,Path,fileSize,string(target.PeerName),fileName).detach(); //dont block the menu waiting on them
    cout<<"Offer sent to "<<target.PeerName<<", you'll get a message when they answer\n";
    return true;

}

bool FormTCP_offer(char* myName,TCP_offer*& ret,string& Path)
{
    ret=nullptr;
    if(myName==nullptr){cerr<<"Nullptr was passed to FormTCP_offer\n"; return false;}

    fstream File;
    while(true)
    {
        cout<<"Enter the Path for the file you want to send: ";
        cin>>ws; //eat the newline left over from the last cin>>
        if(!getline(cin,Path)){if(cin.eof()) return false; clearInput(); continue;} //getline so paths with spaces work

        File.open(Path,ios::binary|ios::in);
        if(File.is_open()) break;

        perror("provided File failed to open");
        int retry=0;
        while(true){
            cout<<"Would you like to try to open the file again? \n Enter 1 for yes.\n Enter 2 for no.\n";
            if(cin>>retry && (retry==1||retry==2)) break;
            if(cin.eof()) return false;
            clearInput();
        }
        if(retry!=1) return false;
        File.clear();
    }

    size_t pos = Path.find_last_of('/');
    string name = (pos == string::npos) ? Path : Path.substr(pos + 1);   // was pos=1

    File.seekg(0, ios::end);
    streamoff size = File.tellg();
    File.seekg(0, ios::beg);
    if(size<0 || name.empty()){cerr<<"That isnt a regular file\n"; return false;} //directories end up here

    TCP_offer* myOffer = new TCP_offer;
    memset(myOffer, 0, sizeof(*myOffer));

    size_t nameCopy = min(name.length(), (size_t)FileNameLen - 1);
    size_t myNameCopy = min(strlen(myName), (size_t)namelen - 1);

    memcpy(myOffer->header, "Offer", 5);
    memcpy(myOffer->SenderName, myName, myNameCopy);
    memcpy(myOffer->fileName, name.c_str(), nameCopy);
    myOffer->fileSize = htobe64((uint64_t)size);


    ret=myOffer;
    return true;

}


//=======================================================
//          Receiving side
//                        start
//=======================================================
string makeSaveName(const TCP_offer& offer)
{
    string name((const char*)offer.fileName,strnlen((const char*)offer.fileName,FileNameLen));
    size_t pos=name.find_last_of('/');
    if(pos!=string::npos) name=name.substr(pos+1); //never trust a path from the sender only keep the file name
    if(name.empty()||name=="."||name=="..") name="received_file";

    string saveName=name;
    int copy=1;
    while(ifstream(saveName).good()) saveName=to_string(copy++)+"_"+name; //dont overwrite anything already here
    return saveName;
}

bool recvFile(int sock,const string& saveName,uint64_t fileSize)
{
    ofstream File(saveName,ios::binary);
    if(!File.is_open()){perror("couldnt create the file"); return false;}

    vector<char> buf(chunkSize);
    uint64_t got=0;
    while(got<fileSize)
    {
        size_t want=min((uint64_t)chunkSize,fileSize-got);
        ssize_t n=recv(sock,buf.data(),want,0);
        if(n<=0)
        {
            if(n==0) cerr<<"\nSender closed the connection early\n";
            else perror("\nrecv failed during transfer");
            File.close();
            remove(saveName.c_str()); //dont leave half a file lying around
            return false;
        }
        File.write(buf.data(),n);
        got+=n;
        printProgress(got,fileSize);
    }
    cout<<"\n";
    return true;
}

bool senderStillWaiting(int sock) //peek without blocking if the sender cancelled their side is closed and recv gives 0
{
    char c;
    ssize_t n=recv(sock,&c,1,MSG_PEEK|MSG_DONTWAIT);
    if(n<0 && (errno==EAGAIN||errno==EWOULDBLOCK)) return true; //nothing to read still sitting there waiting on us
    return n>0;
}

int pruneCancelledOffers() //drops offers the sender cancelled returns how many are still waiting
{
    lock_guard<mutex> offerLock(offerMut);
    int count=incomingOffers.size();
    for(int i=0;i<count;i++) //cycle through once the live ones go back in the same order
    {
        pendingOffer current=incomingOffers.dequeue();
        if(senderStillWaiting(current.sock)) {incomingOffers.enqueue(current); continue;}
        cout<<"\n>> "<<current.offer.SenderName<<" cancelled their offer for "<<(const char*)current.offer.fileName<<"\n";
        close(current.sock);
    }
    return incomingOffers.size();
}

bool RespondToOffers()
{
    if(pruneCancelledOffers()==0){cout<<"No pending offers\n"; return false;}

    pendingOffer current;
    {
        lock_guard<mutex> offerLock(offerMut);
        current=incomingOffers.peek(); //peek not dequeue if they pick later it stays at the front
    }
    //only the menu thread ever dequeues so the front cant change under us the accept thread only adds to the back

    if(debugMode) printOffer(&current.offer);
    cout<<"\n"<<current.offer.SenderName<<" wants to send you "<<(const char*)current.offer.fileName
        <<" ("<<humanSize(be64toh(current.offer.fileSize))<<")\n";

    int answer=0;
    while(true){
        cout<<"Accept this file?\n Enter 1 for yes.\n Enter 2 for no.\n Enter 3 to decide later.\n";
        if(cin>>answer && answer>=1 && answer<=3) break;
        if(cin.eof()) return false;
        clearInput();
    }
    if(answer==3) return false;

    {
        lock_guard<mutex> offerLock(offerMut);
        incomingOffers.dequeue(); //decided take it off the front
    }

    if(!senderStillWaiting(current.sock)) //they cancelled while we were thinking
    {cout<<current.offer.SenderName<<" cancelled this offer\n"; close(current.sock); return false;}

    struct reply myReply;
    memset(&myReply,0,sizeof(myReply));
    memcpy(myReply.header,"Reply",5);
    myReply.Reply= answer==1 ? 1 : 0;

    if(!sendAll(current.sock,&myReply,sizeof(reply)) || answer!=1){close(current.sock); return false;}

    string saveName=makeSaveName(current.offer);
    char cwd[4096];
    if(getcwd(cwd,sizeof(cwd))) cout<<"Saving to "<<cwd<<"/"<<saveName<<"\n"; //files land wherever the program was started from
    else cout<<"Saving as "<<saveName<<"\n";
    bool ok=recvFile(current.sock,saveName,be64toh(current.offer.fileSize));
    close(current.sock);
    if(ok) cout<<"File received\n";
    return ok;
}
//=======================================================
//          Receiving side
//                        end
//=======================================================

int main(int args,char* argv[])
{

    //=====================================================================================
    //      For Args
    //          Argv[0] = program name
    //          Argv[1] = own Peer name
    //          Argv[2] = Debug Mode default no options yes
    //          Argv[3] = broadCast delay in milliSeconds default 500 ms or 5 of a second
    //======================================================================================


    int broadcastDelay=500;
    if(checkValidArgCount(args)) {cerr<<"Please enter a name for your computer\n"; return -1;}
    bool debug=setDebug(args,argv);
    debugMode=debug;
    if(args>3) broadcastDelay=stoi(argv[3]);

    //========================================================
    //      create and bind a socket to a random port
    //                       start
    //========================================================
    int sockfd=socket(AF_INET,SOCK_STREAM,0);
    if(sockfd==-1){perror("socket() call failed for sockfd"); return sockFail;}

    struct sockaddr_storage Myaddr;
    setUpAddrStorage(Myaddr);

    int status=bind(sockfd,(sockaddr *)&Myaddr,sizeof(sockaddr_in));
    if(status==-1){perror("bind call failed\n"); return bindFail;}

    struct sockaddr_storage boundAddr;
    memset(&boundAddr,0,sizeof(boundAddr));
    socklen_t addlen=sizeof(boundAddr);
    status=getsockname(sockfd,(sockaddr*)&boundAddr,&addlen); //stores port in network byte order
    if(status==-1){perror("getsockname call failed\n"); return sockNameFail;}

    struct sockaddr_in* boundAddrIn=(sockaddr_in *)&boundAddr;
    status=listen(sockfd,16);
    if(status==-1){perror("listen call failed\n"); return listenFail;}

    //debug Print
    if(debug)
    cout<<ntohs(boundAddrIn->sin_port)<<"\n";
    //debug Print

    //========================================================
    //      create and bind a socket to a random port
    //                        END
    //========================================================


    //=======================================================
    //   Build my own broadcast message with my name and id
    //                        start
    //=======================================================
    struct broadcast MyBroadCastMessage;
    fillBroadCastStruct(MyBroadCastMessage,argv[1],boundAddrIn->sin_port);
    if(debug)
    displayOwnInfo(MyBroadCastMessage); //sanity print
    //=======================================================
    //   Build my own broadcast message with my name and id
    //                        end
    //=======================================================


    //=======================================================
    //   Make a list of peers and add myself as peer 0
    //                        start
    //=======================================================
    peer Me;
    memset(&Me,0,sizeof(Me));
    for(int i=0;i<idLen;i++) Me.PeerId[i]=MyBroadCastMessage.myid[i];
    for(int i=0;i<namelen;i++)Me.PeerName[i]=MyBroadCastMessage.MyName[i];
    Me.TcpPort=MyBroadCastMessage.myTcp;

    vector<peer> list;
    list.push_back(Me);
    //=======================================================
    //   Make a list of peers and add myself as peer 0
    //                        end
    //=======================================================

    DisplayHeader();

    //=======================================================
    //           set up broadcast addr and socket
    //                        end
    //=======================================================
    int datagramSock=socket(AF_INET,SOCK_DGRAM,0); // new socket exclusively for broadCasts and discovers
    if(datagramSock==-1){perror("Socket() failed for datagramSocket"); return sockFail;}

    sockaddr_storage broadCastAddr;
    memset(&broadCastAddr,0,sizeof(broadCastAddr));
    sockaddr_in * broadCastAddr_in=(sockaddr_in *)&broadCastAddr;
    broadCastAddr_in->sin_port=htons(9999);
    broadCastAddr_in->sin_family=AF_INET;

    status = inet_pton(AF_INET,DiscoverAddr, &(broadCastAddr_in->sin_addr));
    if(status <=0 ){perror("inet_pton failed\n");return ptonFail;}

    int on = 1;
    setsockopt(datagramSock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));  // required to send to a broadcast address
    setsockopt(datagramSock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));  // allow several sockets on 9999
    setsockopt(datagramSock, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));

    timeval tv{1, 0};
    setsockopt(datagramSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));   // recvfrom returns every 1s
    status=bind(datagramSock,(sockaddr*)broadCastAddr_in,sizeof(sockaddr_in));
    if(status==-1){perror("bind failed for datagram socket\n"); return bindFail;}
    //==============================================================
    //           set up broadcast addr and socket
    //                        end
    //==============================================================






    //==============================================================
    //          Set up BroadCast thread
    //                        start
    //==============================================================
    thread broadcastThread([&]()
    {
        while(true){
        Broadcast(datagramSock,broadCastAddr,MyBroadCastMessage);
        this_thread::sleep_for(chrono::milliseconds(broadcastDelay)); //half a sec
        }
    });
    //==============================================================
    //           set up BoradCast thread
    //                        end
    //==============================================================


    //==============================================================
    //          Set up Discover thread
    //                        start
    //==============================================================
    thread discoverThread([&]()
    {
        while(true){
        bool found=false;
        found=Discover(datagramSock,broadCastAddr,MyBroadCastMessage,list);
        if(found && debug){cout<<"\nFound a new Peer\n";}
        this_thread::sleep_for(chrono::milliseconds(broadcastDelay)); //half a sec
        }
    });
    //==============================================================
    //           set up Discover thread
    //                        end
    //==============================================================


    //==============================================================
    //          Set up Accept thread
    //                        start
    //==============================================================
    thread acceptThread([&]()
    {
        while(true){
        int conn=accept(sockfd,nullptr,nullptr); //new socket for every incoming offer sockfd keeps listening
        if(conn==-1){perror("accept failed"); continue;}

        timeval connTimeout{10,0};
        setsockopt(conn,SOL_SOCKET,SO_RCVTIMEO,&connTimeout,sizeof(connTimeout)); //if the sender stalls for 10s give up instead of hanging forever

        pendingOffer incoming;
        memset(&incoming,0,sizeof(incoming));
        if(!recvAll(conn,&incoming.offer,sizeof(TCP_offer)) || memcmp(incoming.offer.header,"Offer",5)!=0)
        {close(conn); continue;} //not an offer not our problem

        incoming.offer.SenderName[namelen-1]='\0';
        incoming.offer.fileName[FileNameLen-1]='\0';
        incoming.sock=conn;

        lock_guard<mutex> offerLock(offerMut);
        incomingOffers.enqueue(incoming);
        cout<<"\n\n>> New file offer from "<<incoming.offer.SenderName<<", pick 3 whenever you want to respond\n";
        }
    });
    //==============================================================
    //           set up Accept thread
    //                        end
    //==============================================================


    bool running=true;
    while (running)
    {

        char choice='\0';;
        int waitingForMe=pruneCancelledOffers();
        int waitingOnThem=0;
        {
            lock_guard<mutex> outLock(outMut);
            waitingOnThem=outgoingOffers.size();
        }
        cout<<"\nMake a selection\n"
            <<"1: list peers\n"
            <<"2: Send a file\n"
            <<"3: Respond to file offers ("<<waitingForMe<<" waiting)\n"
            <<"4: Cancel a sent offer ("<<waitingOnThem<<" pending)\n"
            <<"5: Quit\n";
        cin>>choice;

        if(cin.eof()) break;
        else
        if(cin.fail())
        {
            cerr<<"Invalid entry\n";
            clearInput();
            choice='\0';
            continue;
        }
        cin.ignore(numeric_limits<streamsize>::max(),'\n'); //so typing 12 doesnt turn into 1 then 2



        switch (choice)
        {
        case '1':{
            lock_guard<mutex> listlock(listMut);
            PrintPeers(list);

            break;
        }
        case '2':
            FileTransfer(list);
            break;
        case '3':
            RespondToOffers();
            break;
        case '4':
            CancelOffer();
            break;
        case '5':
            running=false;
            break;
        default:
            cout<<"Invalid entry\n";
            break;
        }


    }

    //the threads loop forever so detach them the process ending takes them down with it
    broadcastThread.detach();
    discoverThread.detach();
    acceptThread.detach();
    close(sockfd);
    close(datagramSock);
    return 0;

}
