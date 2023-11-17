// EchoServer.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <string.h>

#include "global.h"
#include "globalEpoll.h"
#undef BUFFER_SIZE
#define closesocket close

#define IP "127.0.0.1"
#define DEFAULT_PORT 10001
#define Print_ErrCode(e) fprintf(stderr,"\n[Server]%s 执行失败: %d\n",e,WSAGetLastError())
#ifndef DEFAULT_BACKLOG
#define DEFAULT_BACKLOG 128     // backlog太小会导致队列满而使得一些请求被丢弃
#endif
#define MAX_IO_PEND 10

int curr_size = 0; //当前的句柄数
#define OP_READ 0x10
#define OP_WRITE 0x20//定义结构体用于储存通信信息

#define MAX_SEND_SIZE   512
#define BUFFER_SIZE     MAX_SEND_SIZE+1

#ifdef _WIN32
#define LFRP_SEND_FLAGS     0
#else
#define LFRP_SEND_FLAGS     MSG_NOSIGNAL
#endif

int nPort = DEFAULT_PORT;

typedef std::map<int, CVecBuffer> CSocketDataMap;
CSocketDataMap* pSockDataMapAry = nullptr;

int EchoSvrRead(int nIndex, int sock, char* pBuffer, int nCount)
{
    // Accept后同时可读写，也可以通过pSockDataMapAry转来模拟业务异步
    CBuffer buf;
    buf.pBuffer = new char[nCount];
    memcpy(buf.pBuffer, pBuffer, nCount);
    buf.nLen = nCount;
    
    CSocketDataMap::iterator iter = pSockDataMapAry[nIndex].find(sock);
    if (iter != pSockDataMapAry[nIndex].end())
    {
        CVecBuffer& vecBuf = iter->second;
        vecBuf.push_back(buf);
        PRINT_INFO("%s %s,%d: EchoSvrRead socketID %d recv more than one data first size %d and cur size %d\n", GetCurTimeStr(), __FUNCTION__, __LINE__, sock, vecBuf[0].nLen, nCount);
    }
    else
    {
        CVecBuffer vecBuf;
        vecBuf.push_back(buf);
        pSockDataMapAry[nIndex].insert(std::make_pair(sock, vecBuf));
    }
    FireWriteEvent(sock);

    return 0;

    int nRet = send(sock, pBuffer, nCount, LFRP_SEND_FLAGS);
    if (nRet == SOCKET_ERROR && IsReSendSocketError(WSAGetLastError()))
    { // 堵住就等下一个EPOLLOUT事件，清掉已经发送的数据
        PRINT_ERROR("%s %s,%d: send to Tun err size %d wsaerr WSAEWOULDBLOCK\n", GetCurTimeStr(), __FUNCTION__, __LINE__, nCount);
    }
    else if (nRet == SOCKET_ERROR || nRet == 0)
    {
        PRINT_ERROR("%s %s,%d: SocketID %d disconnect because send err %x\n", GetCurTimeStr(), __FUNCTION__, __LINE__, sock, nRet);
    }

    return 0;
}
int EchoSvrWrite(int nIndex, int sock)
{
    CSocketDataMap::iterator iter = pSockDataMapAry[nIndex].find(sock);
    if (iter != pSockDataMapAry[nIndex].end())
    {
        char* pLastBuf = nullptr;
        int nLastLen = 0;
        CVecBuffer& vecBuf = iter->second;
        for (int i = 0; i < vecBuf.size(); i++)
        {
            bool bReSendLast = false;
            CBuffer& buf = vecBuf[i];
            int nRet = send(sock, buf.pBuffer, buf.nLen, LFRP_SEND_FLAGS);
            while (nRet == SOCKET_ERROR && IsReSendSocketError(WSAGetLastError()))
            { // 堵住就等下一个EPOLLOUT事件，清掉已经发送的数据
                PRINT_ERROR("%s %s,%d: send to Tun err size %d wsaerr WSAEWOULDBLOCK\n", GetCurTimeStr(), __FUNCTION__, __LINE__, buf.nLen);
                usleep(1000);
                int nRet = send(sock, buf.pBuffer, buf.nLen, LFRP_SEND_FLAGS);
                //if (pLastBuf)
                //{
                //    nRet = send(sock, pLastBuf, nLastLen, LFRP_SEND_FLAGS);
                //    bReSendLast = true;
                //}
            }
            if (bReSendLast && i > 0)
            { // 如果补发了上一个包，重新进行第i次循环
                i--;
                continue;
            }
            
            if (nRet == SOCKET_ERROR || nRet == 0)
            {
                PRINT_ERROR("%s %s,%d: SocketID %d disconnect because send err %x\n", GetCurTimeStr(), __FUNCTION__, __LINE__, sock, nRet);
            }
            //delete[] buf.pBuffer;
            if (pLastBuf)
            {
                delete[] pLastBuf;
            }
            pLastBuf = buf.pBuffer;
            nLastLen = buf.nLen;
        }
        if (pLastBuf)
        {
            delete[] pLastBuf;
        }
        vecBuf.clear();
        pSockDataMapAry[nIndex].erase(iter);
    }
    
    return 0;
}
int EchoSvrClose(int nIndex, int sock)
{
    //close(sock);
    CloseSocketInstance(nIndex, sock);

    return 0;
}

int EchoSvrTimer()
{
    PRINT_INFO("%s %s,%d: EchoSvrTimer\n", GetCurTimeStr(), __FUNCTION__, __LINE__);
    CommTimer();
    return 0;

}
int mainEchoEpoll()
{
    // 初始化socket
    InitSocket();

    InitLog("./EchoSvr.txt");

    // 初始化epooll工作线程相关
    SetBusWorkerCallBack(EchoSvrRead, EchoSvrWrite, EchoSvrClose, nullptr, nullptr, EchoSvrTimer);
    InitWorkerThreads();
    pSockDataMapAry = new CSocketDataMap[nThreadCount];

    // 监听端口
    SOCKET sockListen = INVALID_SOCKET;
    int nRet = EpollListenSocket(epollfd, sockListen, IP, nPort);
    if (nRet != 0)
    {
        return 1;
    };
    
    mainEpoll(epollfd, sockListen);
    bExitPorcess = true;

    return 0;
}

int main(int argc, char** argv)
{
    int i = 0;
    for (i = 0; i < argc; i++)
    {
        if (strcmp(argv[i], "-p") == 0 && i + 1 <= argc)
        {
            i++;
            nPort = atoi(argv[i]);
        }
    }
    
    mainEchoEpoll();
}