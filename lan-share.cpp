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

using namespace std;

mutex listMutex;

struct peer
{
    char Name[128];
    char ip[INET_ADDRSTRLEN];
    int Sockfd; //udp
    struct sockaddr_storage addr;
    socklen_t addr_len;
    unsigned short port;
    int namelen;
    int vectorIdx;

    struct sockaddr_in broadcastAddr;

    friend ostream& operator << (ostream& out, struct peer P)
    {
        out << P.Name << "\t" << P.ip << "\t" << P.port;
        return out;
    }

    void broadCast()
    {
        const char* msg = "Discover\0";
        const char* msg2 = "akg\0";
        char buffer[10] = {'\0'};

        struct sockaddr_in akgSock;
        memset(&akgSock, 0, sizeof(akgSock));
        socklen_t len = sizeof(akgSock);

        sendto(Sockfd, msg, 9, 0, (const struct sockaddr*)&broadcastAddr, sizeof(broadcastAddr));
        recvfrom(Sockfd, buffer, 4, 0, (sockaddr*)&akgSock, &len);
        char ResponseBuffer[50] = {'\0'};

        if (!strcmp(buffer, msg2)) //akged now send name
        {
            //cout<<"aked"<<endl;//debug

            do
            {
                sendto(Sockfd, Name, strlen(Name) + 1, 0, (sockaddr*)&akgSock, len);
                recv(Sockfd, ResponseBuffer, sizeof(ResponseBuffer), 0);

            } while (strcmp(ResponseBuffer, Name));

            char buffer[] = "Done\0";
            sendto(Sockfd, buffer, sizeof(buffer), 0, (sockaddr*)&akgSock, len);

        } //else cout<<"no akg"<<endl;//debug
    }

    bool static Discover(vector<unique_ptr<peer>>& Peers)
    {
        //int fd=socket(AF_INET,SOCK_DGRAM,0);

        int msgLen = 10;
        int n;
        const char* msg = "Discover\0";
        char buffer[10];

        sockaddr_in addrList;
        memset(&addrList, 0, sizeof(addrList));

        addrList.sin_family = AF_INET;
        addrList.sin_port = htons(9999);
        addrList.sin_addr.s_addr = INADDR_ANY;
        socklen_t len = sizeof(addrList);

        int listen = socket(AF_INET, SOCK_DGRAM, 0);
        int enable = 1;
        setsockopt(listen, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
        setsockopt(listen, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(listen, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        bind(listen, (const sockaddr*)&addrList, len);

        struct sockaddr_storage sender;
        memset(&sender, 0, sizeof(sender));
        socklen_t size = sizeof(sender);

        n = recvfrom(listen, buffer, 10, 0, (sockaddr*)&sender, &size);

        if (n <= 0 || strcmp(msg, buffer))
        {
            close(listen);
            return false;
        }

        sockaddr_in* temp = (sockaddr_in*)&sender;
        const char* msg2 = "akg\0";
        sendto(listen, msg2, 4, 0, (sockaddr*)&sender, size);
        // cout<<msg2<<"SENT"<<endl;

        unique_lock listLock(listMutex);
        //listLock.lock();

        for (int i = 0; i < Peers.size(); i++)
        {
            sockaddr_in* temp2 = (sockaddr_in*)&Peers[i]->addr;
            if (temp->sin_addr.s_addr == temp2->sin_addr.s_addr && temp->sin_port == temp2->sin_port)
            {
                close(listen);
                /*cout<<"\nDUPE FOUND\n"*/;
                return false;
            }
        }

        Peers.push_back(make_unique<peer>(&sender, size, listen, Peers.size(), nullptr));

        listLock.unlock();
        return true;
    }

    peer(struct sockaddr_storage* addr, socklen_t len, int sockfd, int idx, char* Name, bool recordSelf = false)
        : addr(*addr), addr_len(len), Sockfd(sockfd), vectorIdx(idx)
    {
        if (recordSelf && Name != nullptr)
        {
            strcpy(this->Name, Name);
        }
        else if (recordSelf && Name == nullptr)
        {
            backupName(vectorIdx);
        }
        else
        {
            int status = recordName();
            if (status == -1);
            if (!status)
            {
                backupName(vectorIdx);
            }
        }

        if (this->addr.ss_family == AF_INET)
        {
            struct sockaddr_in* ipv4 = (sockaddr_in*)&this->addr;
            inet_ntop(AF_INET, &(ipv4->sin_addr.s_addr), ip, INET_ADDRSTRLEN);
            port = ntohs(ipv4->sin_port);
        }

        int enable = 1;
        setsockopt(Sockfd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));

        memset(&broadcastAddr, 0, sizeof(broadcastAddr));
        broadcastAddr.sin_port = htons(9999);
        broadcastAddr.sin_addr.s_addr = inet_addr("255.255.255.255");
        broadcastAddr.sin_family = AF_INET;

        broadCast();
    }

    bool operator == (const peer& cmp) const
    {
        if (!strcmp((char*)this->ip, (char*)cmp.ip) && this->port == cmp.port) return true;
        else return false;
    }

private:

    void backupName(int vectorIdx)
    {
        perror("Record name failed for peer \n naming it unknown");
        string unknown = "Unkown_" + to_string(vectorIdx);
        char* arr = unknown.data();
        namelen = unknown.length();
        for (int i = 0; i < namelen; i++)
            this->Name[i] = arr[i];
        this->Name[namelen] = '\0';
    }

    int recordName() //this almost never fails | This function is only ever called when a real peer is discovered
    {
        int n;
        char buffer[128];
        const char* msg1 = "Discover\0";
        const char* msg2 = "akg\0";
        char StatusBuffer[50] = {'\0'};
        const char* phrase = "Done\0";

        do
        {
            sockaddr* addrptr = (sockaddr*)&addr;
            socklen_t len = sizeof(addr);

            n = recvfrom(Sockfd, buffer, 128, 0, addrptr, &len); // recieve name
            if (n <= 0) return 0;
            buffer[127] = '\0';

            sendto(Sockfd, buffer, sizeof(buffer), 0, addrptr, len); //send name back
            n = recvfrom(Sockfd, StatusBuffer, 50, 0, addrptr, &len); //recieve confirmation that this is the name
            if (n <= 0) return 0;

        } while (strcmp(StatusBuffer, phrase)); //might get fucked for none null terminated responses
        strcpy(Name, buffer);

        if (!strcmp(buffer, msg1) || !strcmp(buffer, msg2) || !strcmp(buffer, phrase)) return -1;

        strcpy(Name, buffer);
        return 1;
    }
};

class Helpers
{
public:
    void printList(vector<unique_ptr<peer>>& peers) const
    {
        unique_lock listlock(listMutex);

        //listlock.lock();
        size_t size = peers.size();

        for (int i = 0; i < size; i++)
        {
            cout << i + 1 << ":\t" << *peers[i] << std::endl;
        }
        listlock.unlock();
    }
};

int main(int argv, char* args[])
{
    //boradcast name

    if (argv <= 1)
    {
        cout << "Please enter a name for the deivce\n";
        return -1;
    }

    //Basic loop
    //broadcast periodically
    //listen periodically
    //record new discoveries

    struct sockaddr_storage myAddr;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    memset(&myAddr, 0, sizeof(myAddr));

    hints.ai_family = AF_INET;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    struct addrinfo* res = nullptr;

    int status = getaddrinfo(NULL, "0", &hints, &res);

    if (status)
    {
        cout << "getaddrinfo failed\n" << gai_strerror(status) << endl;
    }

    struct addrinfo* head = res;

    while (head != nullptr && head->ai_family != AF_INET)
    {
        head = head->ai_next;
    }

    if (head == nullptr)
    {
        perror("\nNo valid address returned by gai\n");
        exit(-1);
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    status = bind(sockfd, head->ai_addr, head->ai_addrlen);

    if (status)
    {
        perror("\nBind failed\n");
    }

    //peer(struct sockaddr_storage addr, socklen_t len,int sockfd,int idx)

    struct sockaddr_in broadcastAddr;
    socklen_t addrlen = sizeof(broadcastAddr);
    memset(&broadcastAddr, 0, sizeof(broadcastAddr));
    getsockname(sockfd, (sockaddr*)&broadcastAddr, &addrlen);
    broadcastAddr.sin_addr.s_addr = inet_addr("255.255.255.255");
    broadcastAddr.sin_family = AF_INET;

    int enableBroadcast = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &enableBroadcast, sizeof(enableBroadcast));

    sendto(sockfd, args[1], strlen(args[1]) + 1, 0, (const struct sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

    vector<unique_ptr<peer>> list;


    sockaddr_storage selfaddr;
    socklen_t len=sizeof(selfaddr);
    getsockname(sockfd,(sockaddr*)&selfaddr,&len);

    list.push_back(make_unique<peer>(&selfaddr, len, sockfd, list.size(),args[1],true));
    // int a;
    // cout<<list[0]->Name;
    // cin>>a;
    thread broad([&]() { while (1) list[0]->broadCast(); }); //always broadcast local ip
    thread listen([&]() { while (1) list[0]->Discover((list)); });
    Helpers h;

    while (1)
    {
        //i hate myself for starting this bullshit

        int selection;
        cin.clear();
        cout << "press enter to continue\n";
        cin >> selection;
        cin.clear();
        cout << "\033[2J\033[1;1H";

        cout << "========================Welcome To Lan-Share==============================\n"
             << "lan-share allows for easy file sharing over your local network\n"
             << "made by Hammad Rehan\n"
             << "\n\n"
             << "make a Selection"
             << "1: List all discovered devices\n";
        cin >> selection;

        switch (selection)
        {
            default:
                cout << "Invalid Selection\n";
                break;
            case 1:
                h.printList(list);
                break;
        }
    }

    broad.join();
    //listen.join();
}