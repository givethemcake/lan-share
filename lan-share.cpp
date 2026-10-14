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

using namespace std;


//broadCast port is 9999
//broadCast addr is 255.255.255.255
const int addrlen=17;
const char DiscoverAddr[addrlen]="255.255.255.255\0";
const unsigned int port =9999;

const int headerlen=6;
const int namelen=25;
const int idLen=16;

const int sockFail=-2;
const int bindFail=-3;
const int sockNameFail=-4;
const int ptonFail=-5;

#pragma pack(push,1)
struct broadcast
{
    char header[headerlen];
    char MyName[namelen]; 
    uint8_t myid[idLen]; //random 16 digit id 
    uint16_t myTcp; //network protocol
};
#pragma pack(pop)

bool checkValidArgCount(int c)
{
    return c<2;
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

// class PeerList
// {

// }

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

    cout<<"=================================================\n"
        <<"         Welcome to lan-share\n"
        <<"                             Made by Hammad Rehan\n";
    cout<<"=================================================\n";

}

void printPeer(const peer& p)
{
    cout<<"\n";
    cout<<"Name is "<<p.PeerName<<"\n"
        <<"Port is "<<ntohs(p.TcpPort)<<"\n";
    cout<<"Id is ";
    for(int i=0;i<idLen;i++) cout<<(int)p.PeerId[i];
    cout<<"\n";
}

void PrintPeers(const vector<peer>& list)
{
    for(auto iterator=list.begin();iterator!=list.end();iterator++)
        printPeer(*iterator);
        
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

    struct peer *p = new peer;//faah this is a local i hate this shit bro T-T
    //To fix tomorrow make this function return true 
    //get Main to make the peer 
    //we ball for now tho 
    //  TO FIX

    memset(p,0,sizeof(peer));
    memcpy(p->PeerId,MessageRecv.myid,sizeof(p->PeerId));
    memcpy(p->PeerName,MessageRecv.MyName,sizeName);
    memcpy(&p->PeerAddr,&sender,sizeof(sender));

    p->TcpPort=MessageRecv.myTcp;
    list.push_back(*p);
    return true;
    
}



int main(int args,char* argv[])
{
    if(checkValidArgCount(args)) {cerr<<"Please enter a name for your computer\n"; return -1;}
    
    bool debug=setDebug(args,argv);



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
    listen(sockfd,16);

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
    int datagramSock=socket(AF_INET,SOCK_DGRAM,0);
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
    //=======================================================

    while (true)
    {
        bool found=false;
        char choice='\0';;
        cout<<"Make a selection\n"
            <<"1: list peers\n"
            <<"2: Discover peers\n"
            <<"3: Broadcast to peers\n";
        cin>>choice;
        
        if(cin.eof()) break;
        else
        if(cin.fail())
        {
            cerr<<"Invalid entry\n";
            cin.clear();
            choice='\0';
            continue;
        }
        
       

        switch (choice)
        {
        case '1':
            PrintPeers(list);
            break;
        case '2':
            found=Discover(datagramSock,broadCastAddr,MyBroadCastMessage,list);
            if(found && debug){cout<<"\nFound a new Peer\n";}
            break;
        case '3':
            Broadcast(datagramSock,broadCastAddr,MyBroadCastMessage); //need to rename this function have too many broadcasts rn 
            break; 
        default:
            break;
        }

    }

}