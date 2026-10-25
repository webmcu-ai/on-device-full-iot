// =============================================================================
//  server-v004.js  -  classroom bridge for the webMCU-AI Vision trainer (index-v004.html)
//  Run:   npm install ws      then      node server-v004.js
//  Open:  http://localhost:8080          (this server hands out the newest index-vNNN.html from the same folder)
//
//  What it does, in two small jobs:
//   1. Serves the newest index-vNNN.html over plain http, so the page can use ws:// without the https block.
//   2. Bridges the page to boards:  ws://localhost:8080/dev/<board address>[:port]
//      Type that address in section 9 of the page instead of the board's own address. Every message
//      (text and binary) is passed through unchanged in both directions, so debug frames and weights
//      pushes work exactly as if the page were talking to the board directly. Open several tabs, one
//      per board, to watch a whole classroom at once.
//      v004: the bridge itself is unchanged: camera pictures, file pieces and keys are ordinary text messages, so they pass
//      through untouched. What changed: the page is now found by name pattern (index-v*.html, highest number), so a new page
//      version no longer needs a new server. Bluetooth does not use this server at all (the browser talks to the board directly).
//      server-v004.py is the same bridge in Python.
//  Only private network addresses and .local names are allowed, so this is not an open proxy.
//  MIT license. Use at your own risk. By Jeremy Ellis (webmcu-ai).
// =============================================================================
const myHttp = require('http');
const myFs = require('fs');
const myPath = require('path');
const myOs = require('os');
const MyWebSocket = require('ws');

const myPort = Number(process.env.PORT) || 8080;
function myPageName() {                                    // newest index-vNNN.html in this folder
  let myBest = '', myBestNum = -1;
  for (const myFile of myFs.readdirSync(__dirname)) {
    const myMatch = /^index-v(\d+)\.html$/.exec(myFile);
    if (myMatch && Number(myMatch[1]) > myBestNum) { myBestNum = Number(myMatch[1]); myBest = myFile; }
  }
  return myBest;
}
const myBoardPort = 81;                                    // the board's WebSocket port (firmware-v004)
const myAllowedHost = /^(localhost|[A-Za-z0-9-]+\.local|10\.\d+\.\d+\.\d+|192\.168\.\d+\.\d+|172\.(1[6-9]|2\d|3[01])\.\d+\.\d+|169\.254\.\d+\.\d+)$/;

function myLanAddress() {
  const myNets = myOs.networkInterfaces();
  for (const myName in myNets) for (const myNet of myNets[myName]) if (myNet.family === 'IPv4' && !myNet.internal) return myNet.address;
  return 'localhost';
}

const myServer = myHttp.createServer((myReq, myRes) => {
  const myUrl = myReq.url.split('?')[0];
  const myName = myPageName();
  if (myName && (myUrl === '/' || myUrl === '/' + myName)) {
    myFs.readFile(myPath.join(__dirname, myName), (myErr, myData) => {
      if (myErr) { myRes.writeHead(500, { 'Content-Type': 'text/plain' }); myRes.end('Could not read ' + myName); return; }
      myRes.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      myRes.end(myData);
    });
  } else {
    myRes.writeHead(404, { 'Content-Type': 'text/plain' });
    myRes.end('Not found');
  }
});

const myBridge = new MyWebSocket.Server({ noServer: true });

myServer.on('upgrade', (myReq, mySocket, myHead) => {
  const myMatch = /^\/dev\/([A-Za-z0-9.-]+)(?::(\d+))?$/.exec(myReq.url.split('?')[0]);
  if (!myMatch || !myAllowedHost.test(myMatch[1])) { mySocket.destroy(); return; }
  const myTarget = 'ws://' + myMatch[1] + ':' + (myMatch[2] || myBoardPort);
  myBridge.handleUpgrade(myReq, mySocket, myHead, (myBrowser) => {
    const myBoard = new MyWebSocket(myTarget);
    const myWaiting = [];                                   // page messages that arrive before the board answers
    console.log('bridge open: page <-> ' + myTarget);
    myBoard.on('open', () => { myWaiting.forEach(([d, b]) => myBoard.send(d, { binary: b })); myWaiting.length = 0; });
    myBoard.on('message', (myData, myIsBinary) => { if (myBrowser.readyState === 1) myBrowser.send(myData, { binary: myIsBinary }); });
    myBrowser.on('message', (myData, myIsBinary) => {
      if (myBoard.readyState === 1) myBoard.send(myData, { binary: myIsBinary });
      else if (myBoard.readyState === 0) myWaiting.push([myData, myIsBinary]);
    });
    const myCloseBoth = () => { console.log('bridge closed: ' + myTarget); try { myBoard.close(); } catch (e) { /* already closed */ } try { myBrowser.close(); } catch (e) { /* already closed */ } };
    myBoard.on('close', myCloseBoth); myBrowser.on('close', myCloseBoth);
    myBoard.on('error', (myErr) => { console.log('board ' + myTarget + ': ' + myErr.message); myCloseBoth(); });
    myBrowser.on('error', myCloseBoth);
  });
});

myServer.listen(myPort, () => {
  console.log('\n=====================================================');
  console.log(' webMCU-AI bridge running');
  console.log(' Page:    http://localhost:' + myPort + '   (or http://' + myLanAddress() + ':' + myPort + ' from another computer)');
  console.log(' In section 9 of the page use:  ws://localhost:' + myPort + '/dev/xiao-vision.local');
  console.log('                           or:  ws://localhost:' + myPort + '/dev/192.168.1.50');
  console.log('=====================================================\n');
});
