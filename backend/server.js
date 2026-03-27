const http = require('http');
const WebSocket = require('ws');
const fs = require('fs');
const path = require('path');

const PORT = parseInt(process.env.WEB_PORT || '3000');
const MESSAGES_FILE = '/tmp/ft8cb/messages.jsonl';
const MAX_HISTORY = 200; // messages gardés en mémoire

// Stockage en mémoire
let messageHistory = [];
let lastFileSize = 0;
let lastInode = null;

// Serveur HTTP minimal (health check)
const server = http.createServer((req, res) => {
  if (req.url === '/health') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ status: 'ok', messages: messageHistory.length }));
    return;
  }
  res.writeHead(404);
  res.end();
});

// WebSocket server
const wss = new WebSocket.Server({ server });

wss.on('connection', (ws, req) => {
  console.log(`[ws] Client connected from ${req.socket.remoteAddress}`);

  // Envoyer l'historique au nouveau client
  ws.send(JSON.stringify({
    type: 'history',
    messages: messageHistory
  }));

  ws.on('close', () => console.log('[ws] Client disconnected'));
  ws.on('error', (e) => console.error('[ws] Error:', e.message));
});

function broadcast(msg) {
  const data = JSON.stringify({ type: 'message', data: msg });
  wss.clients.forEach(client => {
    if (client.readyState === WebSocket.OPEN) {
      client.send(data);
    }
  });
}

// Surveille le fichier JSONL pour les nouveaux messages
function watchMessages() {
  // Crée le dossier/fichier si pas encore là
  const dir = path.dirname(MESSAGES_FILE);
  if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
  if (!fs.existsSync(MESSAGES_FILE)) fs.writeFileSync(MESSAGES_FILE, '');

  setInterval(() => {
    try {
      const stat = fs.statSync(MESSAGES_FILE);

      // Fichier rotaté (inode changé ou taille réduite)
      if (stat.ino !== lastInode || stat.size < lastFileSize) {
        lastFileSize = 0;
        lastInode = stat.ino;
      }

      if (stat.size <= lastFileSize) return;

      // Lire seulement les nouveaux octets
      const fd = fs.openSync(MESSAGES_FILE, 'r');
      const buf = Buffer.alloc(stat.size - lastFileSize);
      fs.readSync(fd, buf, 0, buf.length, lastFileSize);
      fs.closeSync(fd);

      lastFileSize = stat.size;
      lastInode = stat.ino;

      // Parser les lignes JSON
      const lines = buf.toString('utf8').split('\n').filter(l => l.trim());
      lines.forEach(line => {
        try {
          const msg = JSON.parse(line);
          // Ajouter timestamp de réception serveur
          msg.received_at = new Date().toISOString();
          messageHistory.push(msg);
          if (messageHistory.length > MAX_HISTORY) messageHistory.shift();
          broadcast(msg);
          console.log(`[msg] ${msg.ts} ${msg.msg} SNR=${msg.snr}`);
        } catch (e) {
          // ligne incomplète, ignorer
        }
      });
    } catch (e) {
      // fichier pas encore créé
    }
  }, 500); // poll toutes les 500ms
}

server.listen(PORT, '0.0.0.0', () => {
  console.log(`[ft8cb-backend] WebSocket server listening on port ${PORT}`);
  watchMessages();
});
