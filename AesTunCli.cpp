// AesTunCli.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <vector>
#include <atomic>
#include "aes.h"
#include "global.h"
#include "globalEpoll.h"

#pragma comment(lib,"ws2_32.lib")

CSocketPairMap* pSocketPairMapAry = nullptr;    // 提供服务和访问者对数组，一个工作线程使用一个Map

// 本地服务监听端口信息
std::string strHost = "127.0.0.1";
int nPort = 12345; //808;
// 后端服务信息
std::string strSvrHost = "127.0.0.1";
int nSvrPort = 6868;

// AES key
std::string strDefaultKey = "asdf1234567890";
std::string strAesKey = strDefaultKey;

void CloseSocketPair(int nIndex, int key)
{
    CSocketPairMap::iterator iter = pSocketPairMapAry[nIndex].find(key);
    if (iter != pSocketPairMapAry[nIndex].end())
    {
        CSocketPair& pair = iter->second;
        pSocketPairMapAry[nIndex].erase(iter);
        CloseSocketPair(pair);
    }
}

std::atomic<int> nVIDCreator(0);
// 注意CliPostAccept是主线程调用，尽量简单；另外这个需要比EpollAdd早，可以在所有消息之前处理事件
int CliPostAccept(CLfrpSocket* pSocket)
{
    if (pSocket == nullptr)
        return -1;

    //pSocket->nSocketID = pSocket->sock;             // 设置nSocketID
    pSocket->nServiceNumber = nVIDCreator++;
    if (pSocket->nServiceNumber == INVALID_SOCKET)
    { // 0是默认值，不使用，循环到了重新分配
        pSocket->nServiceNumber = nVIDCreator++;
    }
    pSocket->nSocketID = pSocket->nServiceNumber;
    
    PRINT_INFO("%s %s,%d: PoseAccept SocketID %d use VID %d \n", GetCurTimeStr(), __FUNCTION__, __LINE__, pSocket->sock, pSocket->nServiceNumber);
    // accept后马上确定实际工作线程了，转过去不要做任何业务，这样实际业务处理部分都是无锁的
    FireTransEvent(pSocket->sock); 
    return 0;
}

// 在对应对应线程连接connect，并且新建连接也会在相同线程处理
int CliTrans(int nIndex, int sock, CLfrpSocket* pSocket)
{
    if (pSocket == nullptr)
    {
        pSocket = GetSockFromInstanceMap(sock);
    }
    if (pSocket == nullptr)
    {
        PRINT_ERROR("%s Tun %s,%d: sock %d cant find instance\n", GetCurTimeStr(), __FUNCTION__, __LINE__, sock);
        return 0;
    }

    CLfrpSocket* pSvrSocket = new CLfrpSocket;
    pSvrSocket->nServiceNumber = pSocket->nServiceNumber;
    CSocketPair pair;
    pair.pVistor = pSocket;
    pair.pServer = pSvrSocket;
    // 新的Client连接过来，配套一条连到业务服务
    if (PreConnectSocket(pSvrSocket->sock, strSvrHost.c_str(), nSvrPort) != 0)
    {
        printf("%s connect() Svr Failed: %d\n", GetCurTimeStr(), WSAGetLastError());
        CloseServerSocket(pair);
    }
    else
    {
        PRINT_INFO("%s %s,%d: SvrRead connect sock %d for SocketID %d\n", GetCurTimeStr(), __FUNCTION__, __LINE__, pSvrSocket->sock, pSvrSocket->nSocketID);
        // 先把sock放到管理对象中
        AddSockToInstanceMap(pSvrSocket->sock, pSvrSocket);    // 添加到实例，这里务必比SvrWrite先完成
        pSocketPairMapAry[nIndex].insert(std::make_pair(pSocket->nServiceNumber, pair));

        // 实际连接
        int nRet = ProcssConnectSocket(pSvrSocket->sock, strSvrHost.c_str(), nSvrPort);
        nRet = EpollPostConnectSocket(epollfd, pSvrSocket->sock, strSvrHost.c_str(), nSvrPort, nRet);
        if (nRet < 0)
        {
            CloseSocketPair(nIndex, pSocket->nServiceNumber);
        }
    }
    
    return 0;
}

int CliRead(int nIndex, int sock, char* pBuffer, int nCount)
{
    CLfrpSocket* pSocket = GetSockFromInstanceMap(sock);
    if (pSocket)
    {
        CSocketPairMap::iterator iter = pSocketPairMapAry[nIndex].find(pSocket->nServiceNumber);
        if (iter != pSocketPairMapAry[nIndex].end())
        {
            CSocketPair& pair = iter->second;
            if (pSocket == pair.pVistor && pair.pServer)
            { // 收到请求
                RecordSocketData(RECORD_TYPE_CTUN_RECV, sock, pBuffer, nCount);
                pSocket->nLastRecvSec = GetCurSecond();
                int nSeq = GetNextSeq(nIndex, SEQ_CLIENT, pSocket->sock);
                //PRINT_INFO("%s Cli %s,%d: Svr socketID %d recv pack size %d seq %d\n", GetCurTimeStr(), __FUNCTION__, __LINE__, pSocket->sock, nCount, nSeq);
                // todo, 注释掉下面的代码
                //std::string str(pBuffer, nCount);
                //PRINT_INFO("%s Cli %s,%d: Svr socketID %d recv pack size %d:%s seq %d\n", GetCurTimeStr(), __FUNCTION__, __LINE__, pSocket->nSocketID, nCount, str.c_str(), nSeq);
                AesTunMakeSendPack(pair.pServer, pBuffer, nCount, nSeq);
                // todo, 可能连接业务的connect还没完全，发送写事件导致Svr服务异常，需要Svr连接完成后发回到Cli置状态
                FireWriteEvent(pair.pServer->sock);
            }
            else if (pSocket == pair.pServer&& pair.pVistor )
            { // 收到回复
                RecordSocketData(RECORD_TYPE_STUN_RECV, sock, pBuffer, nCount);
                AesTunRecvAndMoveDate(pSocket, pair.pVistor, pBuffer, nCount, pair);
            }
        }
    }
    
    return 0;
}

int CliWrite(int nIndex, int sock)
{
    CLfrpSocket* pSocket = GetSockFromInstanceMap(sock);
    if (pSocket)
    {
        CSocketPairMap::iterator iter = pSocketPairMapAry[nIndex].find(pSocket->nServiceNumber);
        if (iter != pSocketPairMapAry[nIndex].end())
        {
            CSocketPair& pair = iter->second;
            if (pSocket == pair.pVistor)
            { // 发送回复
                AesTunSendDate(pSocket, false, false, pair);
            }
            else
            { // 发送请求
                AesTunSendDate(pSocket, true, true, pair);
            }

            // 如果发送堵住了，下次重新发送
            //if (pSocket->vecSendBuf.size() > 0)
            {
            //    usleep(1000);
            //    FireWriteEvent(pSocket->sock);
            }
        }
    }

    return 0;
}

int CliClose(int nIndex, int sock)
{
    CLfrpSocket* pSocket = GetSockFromInstanceMap(sock);
    if (pSocket == nullptr)
    {
        //PRINT_INFO("%s Tun %s,%d: SocketID %d cant find instance\n", GetCurTimeStr(), __FUNCTION__, __LINE__, sock);
        return 0;
    }

    PRINT_INFO("%s %s,%d: CliClose sock %d\n", GetCurTimeStr(), __FUNCTION__, __LINE__, sock);
    CSocketPairMap::iterator iter = pSocketPairMapAry[nIndex].find(pSocket->nServiceNumber);
    if (iter != pSocketPairMapAry[nIndex].end())
    {
        CSocketPair& pair = iter->second;
        CloseSocketPair(pair);
    }
    
    return -1;
}

int CliTimer()
{
    CommTimer();
    return 0;
}

int main(int argc, char** argv)
{
    int nRet = 0;
    PRINT_ERROR("%s Cli used as 'AesTunCli -p ListenPort -sh ServerHost -sp ServerPort -k AESKey', default is 'AesTunCli -p %d -sh %s -sp %d -k %s'\n", GetCurTimeStr(), nPort, strSvrHost, nSvrPort, strAesKey.c_str());
    int i = 0;
    for (i = 0; i < argc; i++)
    {
        if (strcmp(argv[i], "-h") == 0 && i + 1 <= argc)
        {
            i++;
            strHost = argv[i];
        }
        else if (strcmp(argv[i], "-p") == 0 && i + 1 <= argc)
        {
            i++;
            nPort = atoi(argv[i]);
        }
        else if (strcmp(argv[i], "-sh") == 0 && i + 1 <= argc)
        {
            i++;
            strSvrHost = argv[i];
        }
        else if (strcmp(argv[i], "-sp") == 0 && i + 1 <= argc)
        {
            i++;
            nSvrPort = atoi(argv[i]);
        }
        else if (strcmp(argv[i], "-k") == 0 && i + 1 <= argc)
        {
            i++;
            strAesKey = argv[i];
        }
    }

    if (strAesKey == strDefaultKey)
    {
        PRINT_ERROR("Warning: you should change default aes key!!!\n");
    }

    // 初始化AES密钥信息
    CAES::GlobalInit(strAesKey.c_str());

    // 初始化socket
    InitSocket();
    
    InitSection("Cli");
    InitLog("./Cli.txt");
    // 初始化epooll工作线程相关
    SetBusWorkerCallBack(CliRead, CliWrite, CliClose, CliTrans, CliPostAccept, CliTimer);
    InitWorkerThreads();
    pSocketPairMapAry = new CSocketPairMap[nThreadCount];

    // 监听端口
    SOCKET sockListen = INVALID_SOCKET;
    nRet = EpollListenSocket(epollfd, sockListen, strHost.c_str(), nPort);
    if (nRet != 0)
    {
        return 1;
    };

    mainEpoll(epollfd, sockListen);
    bExitPorcess = true;
}

