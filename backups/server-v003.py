#!/usr/bin/env python3
# =============================================================================
#  server-v003.py  -  classroom bridge for the webMCU-AI Vision trainer (index-v003.html)
#  This is the Python twin of server-v003.js: same jobs, same addresses, so use whichever you have installed.
#  Run:   pip install aiohttp      then      python server-v003.py
#  Open:  http://localhost:8080          (this server hands out index-v003.html from the same folder)
#
#  What it does, in two small jobs:
#   1. Serves index-v003.html over plain http, so the page can use ws:// without the https block.
#   2. Bridges the page to boards:  ws://localhost:8080/dev/<board address>[:port]
#      Type that address in section 9 of the page instead of the board's own address. Every message
#      (text and binary) is passed through unchanged in both directions, so camera pictures (@snap),
#      remote keys, debug frames and weights pushes work exactly as if the page talked to the board directly.
#      Open several tabs, one per board, to watch a whole classroom at once.
#  Only private network addresses and .local names are allowed, so this is not an open proxy.
#  MIT license. Use at your own risk. By Jeremy Ellis (webmcu-ai).
# =============================================================================
import asyncio
import os
import re
import socket

from aiohttp import ClientSession, WSMsgType, web

myPort = int(os.environ.get('PORT', '8080'))
myFolder = os.path.dirname(os.path.abspath(__file__))
myPagePath = os.path.join(myFolder, 'index-v003.html')
myBoardPort = 81                                           # the board's WebSocket port (firmware-v003)
myAllowedHost = re.compile(r'^(localhost|[A-Za-z0-9-]+\.local|10\.\d+\.\d+\.\d+|192\.168\.\d+\.\d+|172\.(1[6-9]|2\d|3[01])\.\d+\.\d+|169\.254\.\d+\.\d+)$')
myTargetPattern = re.compile(r'^([A-Za-z0-9.-]+)(?::(\d+))?$')


def myLanAddress():
    # The address other computers can use to reach this one (no data is sent by this trick).
    try:
        mySock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        mySock.connect(('10.255.255.255', 1))
        myAddress = mySock.getsockname()[0]
        mySock.close()
        return myAddress
    except OSError:
        return 'localhost'


async def myServePage(myRequest):
    try:
        with open(myPagePath, 'rb') as myFile:
            myData = myFile.read()
    except OSError:
        return web.Response(status=500, text='index-v003.html must be in the same folder as server-v003.py')
    return web.Response(body=myData, content_type='text/html', charset='utf-8')


async def myBridge(myRequest):
    myMatch = myTargetPattern.match(myRequest.match_info['target'])
    if not myMatch or not myAllowedHost.match(myMatch.group(1)):
        return web.Response(status=403, text='Only private network addresses and .local names are allowed')
    myTarget = 'ws://' + myMatch.group(1) + ':' + (myMatch.group(2) or str(myBoardPort))

    myBrowser = web.WebSocketResponse(max_msg_size=0)      # 0 = no size limit (camera pictures are big lines)
    await myBrowser.prepare(myRequest)
    async with ClientSession() as mySession:
        try:
            myBoard = await mySession.ws_connect(myTarget, max_msg_size=0)
        except Exception as myError:
            print('board ' + myTarget + ': ' + str(myError))
            await myBrowser.close()
            return myBrowser
        print('bridge open: page <-> ' + myTarget)

        async def myToBoard():
            async for myMsg in myBrowser:
                if myMsg.type == WSMsgType.TEXT:
                    await myBoard.send_str(myMsg.data)
                elif myMsg.type == WSMsgType.BINARY:
                    await myBoard.send_bytes(myMsg.data)
                else:
                    break

        async def myToBrowser():
            async for myMsg in myBoard:
                if myMsg.type == WSMsgType.TEXT:
                    await myBrowser.send_str(myMsg.data)
                elif myMsg.type == WSMsgType.BINARY:
                    await myBrowser.send_bytes(myMsg.data)
                else:
                    break

        myTasks = [asyncio.ensure_future(myToBoard()), asyncio.ensure_future(myToBrowser())]
        try:
            await asyncio.wait(myTasks, return_when=asyncio.FIRST_COMPLETED)
        finally:
            for myTask in myTasks:
                myTask.cancel()
            await asyncio.gather(*myTasks, return_exceptions=True)
            await myBoard.close()
            await myBrowser.close()
            print('bridge closed: ' + myTarget)
    return myBrowser


def myMakeApp():
    myApp = web.Application()
    myApp.router.add_get('/', myServePage)
    myApp.router.add_get('/index-v003.html', myServePage)
    myApp.router.add_get('/dev/{target}', myBridge)
    return myApp


if __name__ == '__main__':
    print('\n=====================================================')
    print(' webMCU-AI bridge (Python) running')
    print(' Page:    http://localhost:' + str(myPort) + '   (or http://' + myLanAddress() + ':' + str(myPort) + ' from another computer)')
    print(' In section 9 of the page use:  ws://localhost:' + str(myPort) + '/dev/xiao-vision.local')
    print('                           or:  ws://localhost:' + str(myPort) + '/dev/192.168.1.50')
    print('=====================================================\n')
    web.run_app(myMakeApp(), port=myPort, print=None)
