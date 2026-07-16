const temperatureElement = document.getElementById("temperature");
const targetElement = document.getElementById("targetTemp");
const powerElement = document.getElementById("heatingPower");
const pTermElement = document.getElementById("pTerm");
const dTermElement = document.getElementById("dTerm");
const iTermElement = document.getElementById("iTerm");
const statusElement = document.getElementById("updateStatus");
const setTargetButton = document.getElementById("setTarget");
const loggingButton = document.getElementById("beginLogging");
const logList = document.getElementById("logList");
const downloadAllButton = document.getElementById("downloadAllLogs");
const deleteAllButton = document.getElementById("deleteAllLogs");
let logging = false;
let db = null;
let tempChart = null;
let currentLogSessionId = null;
let selectedLogId = null;
const maxChartPoints = 60;
let socket = null;
let reconnectTimer = null;
const WS_RECONNECT_MS = 5000;

const request = indexedDB.open("tempLoggerDB", 4);
request.onupgradeneeded = (event) => {
  const db = event.target.result;
  const transaction = event.target.transaction;

  if (!db.objectStoreNames.contains("temperatureLogs")) {
    const objectStore = db.createObjectStore("temperatureLogs", { keyPath: "id", autoIncrement: true });
    objectStore.createIndex("logId", "logId", { unique: false });
    objectStore.createIndex("timestamp", "timestamp", { unique: false });
    objectStore.createIndex("temperature", "temperature", { unique: false });
    objectStore.createIndex("target", "target", { unique: false });
    objectStore.createIndex("heatingPower", "heatingPower", { unique: false });
    objectStore.createIndex("pTerm", "pTerm", { unique: false });
    objectStore.createIndex("dTerm", "dTerm", { unique: false });
    objectStore.createIndex("iTerm", "iTerm", { unique: false });
  } else {
    const objectStore = transaction.objectStore("temperatureLogs");
    if (!objectStore.indexNames.contains("logId")) {
      objectStore.createIndex("logId", "logId", { unique: false });
    }
    if (!objectStore.indexNames.contains("timestamp")) {
      objectStore.createIndex("timestamp", "timestamp", { unique: false });
    }
    if (!objectStore.indexNames.contains("temperature")) {
      objectStore.createIndex("temperature", "temperature", { unique: false });
    }
    if (!objectStore.indexNames.contains("target")) {
      objectStore.createIndex("target", "target", { unique: false });
    }
    if (!objectStore.indexNames.contains("heatingPower")) {
      objectStore.createIndex("heatingPower", "heatingPower", { unique: false });
    }
    if (!objectStore.indexNames.contains("pTerm")) {
      objectStore.createIndex("pTerm", "pTerm", { unique: false });
    }
    if (!objectStore.indexNames.contains("dTerm")) {
      objectStore.createIndex("dTerm", "dTerm", { unique: false });
    }
    if (!objectStore.indexNames.contains("iTerm")) {
      objectStore.createIndex("iTerm", "iTerm", { unique: false });
    }
  }

  if (!db.objectStoreNames.contains("logSessions")) {
    const sessionStore = db.createObjectStore("logSessions", { keyPath: "id", autoIncrement: true });
    sessionStore.createIndex("createdAt", "createdAt", { unique: false });
  } else {
    const sessionStore = transaction.objectStore("logSessions");
    if (!sessionStore.indexNames.contains("createdAt")) {
      sessionStore.createIndex("createdAt", "createdAt", { unique: false });
    }
  }
};

request.onsuccess = (event) => {
  db = event.target.result;
  loadSessions();
};

request.onerror = (event) => {
  console.error("IndexedDB error:", event.target.errorCode);
  setStatus("Database unavailable", true);
};

function setStatus(message, error = false) {
  statusElement.textContent = message;
  statusElement.classList.toggle("error", error);
}

function formatTime(timestamp) {
  const date = new Date(timestamp);
  return date.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

function createNewLogSession() {
  if (!db) {
    alert('Database is not ready yet.');
    return;
  }

  const createdAt = new Date().toISOString();
  const sessionName = `Log ${createdAt.slice(0, 19).replace("T", " ").replace(/:/g, "-")}`;
  const session = {
    name: sessionName,
    createdAt,
    endedAt: null,
    status: "active",
  };

  const transaction = db.transaction(["logSessions"], "readwrite");
  const store = transaction.objectStore("logSessions");
  const request = store.add(session);

  request.onsuccess = (event) => {
    currentLogSessionId = event.target.result;
    setStatus(`Logging started: ${sessionName}`, false);
    loadSessions();
  };

  request.onerror = (event) => {
    console.error("Failed to create log session:", event.target.error);
    alert('Unable to start a new log session.');
  };
}

function closeCurrentLogSession() {
  if (!db || currentLogSessionId === null) {
    return;
  }

  const transaction = db.transaction(["logSessions"], "readwrite");
  const store = transaction.objectStore("logSessions");
  const getRequest = store.get(currentLogSessionId);

  getRequest.onsuccess = (event) => {
    const session = event.target.result;
    if (!session) {
      currentLogSessionId = null;
      return;
    }

    session.endedAt = new Date().toISOString();
    session.status = "complete";

    const updateRequest = store.put(session);
    updateRequest.onsuccess = () => {
      setStatus(`Logging stopped: ${session.name}`, false);
      currentLogSessionId = null;
      loadSessions();
    };

    updateRequest.onerror = (event) => {
      console.error("Failed to close log session:", event.target.error);
    };
  };

  getRequest.onerror = (event) => {
    console.error("Failed to get log session:", event.target.error);
  };
}

function logTemperature(db, temperature, target, heatingPower, pTerm, dTerm, iTerm) {
  if (!db || currentLogSessionId === null) {
    return;
  }

  const transaction = db.transaction(["temperatureLogs"], "readwrite");
  const objectStore = transaction.objectStore("temperatureLogs");
  const data = {
    timestamp: new Date().toISOString(),
    temperature,
    target,
    heatingPower,
    pTerm,
    dTerm,
    iTerm,
    logId: currentLogSessionId,
  };

  const request = objectStore.add(data);
  request.onsuccess = () => {
    console.debug("Temperature logged:", data);
  };
  request.onerror = (event) => {
    console.error("Error logging temperature:", event.target.error);
  };
}

function addChartPoint(label, value) {
  if (!tempChart) {
    return;
  }

  tempChart.data.labels.push(label);
  tempChart.data.datasets[0].data.push(value);

  if (tempChart.data.labels.length > maxChartPoints) {
    tempChart.data.labels.shift();
    tempChart.data.datasets[0].data.shift();
  }

  tempChart.update("none");
}

function updateDisplay(currentTemp, targetTemp, heatingPower, pTerm, dTerm, iTerm) {
  temperatureElement.textContent = `${currentTemp.toFixed(1)} °C`;
  targetElement.textContent = `${targetTemp.toFixed(1)} °C`;
  powerElement.textContent = `${Math.round(heatingPower * 100)} %`;
  pTermElement.textContent = `${Math.round(pTerm * 100)} %`;
  dTermElement.textContent = `${Math.round(dTerm * 100)} %`;
  iTermElement.textContent = `${Math.round(iTerm * 100)} %`;

  setStatus(currentLogSessionId ? `Logging: ${formatTime(new Date())}` : "Live data streaming", false);
  addChartPoint(formatTime(new Date()), currentTemp);

  if (logging) {
    logTemperature(db, currentTemp, targetTemp, heatingPower, pTerm, dTerm, iTerm);
  }
}

function handleSocketMessage(event) {
  try {
    const message = JSON.parse(event.data);
    updateDisplay(
      message.temperature,
      message.target,
      message.heatingPower,
      message.pTerm ?? 0,
      message.dTerm ?? 0,
      message.iTerm ?? 0,
    );
  } catch (error) {
    console.error('Invalid websocket data', error);
  }
}

function scheduleReconnect() {
  if (reconnectTimer) {
    return;
  }

  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connectWebSocket();
  }, WS_RECONNECT_MS);
}

function connectWebSocket() {
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  const url = `${protocol}//${window.location.host}/ws`;

  if (socket && socket.readyState === WebSocket.OPEN) {
    return;
  }

  socket = new WebSocket(url);

  socket.addEventListener('open', () => {
    setStatus('Connected to server', false);
  });

  socket.addEventListener('message', handleSocketMessage);

  socket.addEventListener('close', () => {
    setStatus('Websocket disconnected', true);
    scheduleReconnect();
  });

  socket.addEventListener('error', (error) => {
    console.error('Websocket error', error);
    setStatus('Websocket error', true);
    socket.close();
  });
}

function sendTargetTemperature() {
  const tempValue = document.getElementById('targetInput').value;
  if (tempValue === "" || Number.isNaN(Number(tempValue))) {
    alert('Please enter a valid target temperature.');
    return;
  }

  fetch('/setTargetTemperature', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/x-www-form-urlencoded',
    },
    body: 'target=' + encodeURIComponent(tempValue),
  })
    .then((response) => {
      if (!response.ok) {
        throw new Error('Failed to set target temperature');
      }
      return response.text();
    })
    .then((data) => {
      targetElement.textContent = `${parseFloat(data).toFixed(1)} °C`;
      setStatus('Target updated', false);
    })
    .catch((error) => {
      console.error('Error:', error);
      setStatus('Failed to set target temperature', true);
      alert('Failed to set target temperature.');
    });
}

function initChart() {
  const ctx = document.getElementById('tempChart').getContext('2d');
  tempChart = new Chart(ctx, {
    type: 'line',
    data: {
      labels: [],
      datasets: [{
        label: 'Temperature (°C)',
        data: [],
        borderColor: 'rgba(100, 210, 255, 0.95)',
        backgroundColor: 'rgba(100, 210, 255, 0.18)',
        fill: true,
        tension: 0.25,
        pointRadius: 0,
        borderWidth: 2,
      }],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      plugins: {
        legend: {
          display: false,
        },
      },
      scales: {
        x: {
          title: {
            display: true,
            text: 'Time',
            color: '#a6b4d8',
          },
          ticks: {
            color: '#9aa5c8',
            maxRotation: 0,
            autoSkip: true,
            maxTicksLimit: 8,
          },
          grid: {
            color: 'rgba(255,255,255,0.06)',
          },
        },
        y: {
          title: {
            display: true,
            text: 'Temperature (°C)',
            color: '#a6b4d8',
          },
          ticks: {
            color: '#9aa5c8',
          },
          grid: {
            color: 'rgba(255,255,255,0.06)',
          },
        },
      },
    },
  });
}

function loadSessions() {
  if (!db) {
    return;
  }

  const transaction = db.transaction(["logSessions"], "readonly");
  const store = transaction.objectStore("logSessions");
  const request = store.getAll();

  request.onsuccess = (event) => {
    const sessions = event.target.result;
    renderSessions(sessions.sort((a, b) => new Date(b.createdAt) - new Date(a.createdAt)));
  };

  request.onerror = (event) => {
    console.error('Failed to load log sessions:', event.target.error);
  };
}

function renderSessions(sessions) {
  if (!logList) {
    return;
  }

  if (sessions.length === 0) {
    logList.innerHTML = '<div class="log-row"><div class="log-info"><div class="log-name">No log sessions yet</div><div class="log-meta">Press Begin Logging to start a new log file.</div></div></div>';
    return;
  }

  logList.innerHTML = '';

  sessions.forEach((session) => {
    const row = document.createElement('div');
    row.className = 'log-row';
    if (session.id === selectedLogId) {
      row.classList.add('selected');
    }

    const info = document.createElement('div');
    info.className = 'log-info';

    const title = document.createElement('div');
    title.className = 'log-name';
    title.textContent = session.name;

    const status = document.createElement('div');
    status.className = 'log-meta';
    const created = new Date(session.createdAt).toLocaleString();
    const ended = session.endedAt ? new Date(session.endedAt).toLocaleString() : 'still active';
    status.textContent = `${session.status.toUpperCase()} • started ${created} • ended ${ended}`;

    info.appendChild(title);
    info.appendChild(status);

    const actions = document.createElement('div');
    actions.className = 'log-actions';

    const downloadButton = document.createElement('button');
    downloadButton.textContent = 'Download CSV';
    downloadButton.addEventListener('click', (event) => {
      event.stopPropagation();
      downloadSessionCsv(session.id, session.name);
    });

    const deleteButton = document.createElement('button');
    deleteButton.textContent = 'Delete';
    deleteButton.className = 'danger';
    deleteButton.addEventListener('click', (event) => {
      event.stopPropagation();
      deleteSession(session.id);
    });

    actions.appendChild(downloadButton);
    actions.appendChild(deleteButton);

    row.appendChild(info);
    row.appendChild(actions);

    row.addEventListener('click', () => {
      selectedLogId = selectedLogId === session.id ? null : session.id;
      renderSessions(sessions);
    });

    logList.appendChild(row);
  });
}

function getLogsBySession(sessionId) {
  return new Promise((resolve, reject) => {
    if (!db) {
      reject(new Error('Database not ready')); return;
    }

    const transaction = db.transaction(["temperatureLogs"], "readonly");
    const store = transaction.objectStore("temperatureLogs");
    const index = store.index("logId");
    const request = index.getAll(IDBKeyRange.only(sessionId));

    request.onsuccess = (event) => {
      resolve(event.target.result.sort((a, b) => new Date(a.timestamp) - new Date(b.timestamp)));
    };

    request.onerror = (event) => {
      reject(event.target.error);
    };
  });
}

function getAllSessions() {
  return new Promise((resolve, reject) => {
    if (!db) {
      reject(new Error('Database not ready')); return;
    }

    const transaction = db.transaction(["logSessions"], "readonly");
    const store = transaction.objectStore("logSessions");
    const request = store.getAll();

    request.onsuccess = (event) => {
      resolve(event.target.result);
    };

    request.onerror = (event) => {
      reject(event.target.error);
    };
  });
}

function downloadSessionCsv(sessionId, sessionName) {
  getLogsBySession(sessionId)
    .then((logs) => {
      if (logs.length === 0) {
        alert('No records found for this session.');
        return;
      }

      const headers = ['id', 'timestamp', 'temperature', 'target', 'heatingPower', 'pTerm', 'dTerm', 'iTerm'];
      const csvRows = [headers.join(',')];
      logs.forEach((log) => {
        const row = [log.id, log.timestamp, log.temperature, log.target, log.heatingPower, log.pTerm, log.dTerm, log.iTerm];
        csvRows.push(row.join(','));
      });

      downloadCSV(csvRows.join('\n'), `${sessionName}.csv`);
    })
    .catch((error) => {
      console.error('Failed to download session CSV:', error);
      alert('Unable to download this log right now.');
    });
}

function downloadAllSessions() {
  getAllSessions()
    .then((sessions) => {
      if (sessions.length === 0) {
        alert('No log sessions available.');
        return;
      }

      const downloadPromises = sessions.map((session) =>
        getLogsBySession(session.id).then((logs) => ({ session, logs }))
      );

      Promise.all(downloadPromises)
        .then((items) => {
          const rows = [];
          rows.push('sessionName,createdAt,endedAt,logId,timestamp,temperature,target,heatingPower,pTerm,dTerm,iTerm');

          items.forEach(({ session, logs }) => {
            logs.forEach((log) => {
              const row = [
                session.name,
                session.createdAt,
                session.endedAt || '',
                log.id,
                log.timestamp,
                log.temperature,
                log.target,
                log.heatingPower,
                log.pTerm,
                log.dTerm,
                log.iTerm,
              ];
              rows.push(row.join(','));
            });
          });

          const filename = `all_logs_${new Date().toISOString().slice(0, 19).replace(/:/g, '-')}.csv`;
          downloadCSV(rows.join('\n'), filename);
        })
        .catch((error) => {
          console.error('Error downloading all logs:', error);
          alert('Unable to download all logs right now.');
        });
    })
    .catch((error) => {
      console.error('Failed to load sessions for download:', error);
      alert('Unable to download all logs right now.');
    });
}

function deleteSession(sessionId) {
  if (!confirm('Are you sure you want to delete this log session? This cannot be undone.')) {
    return;
  }

  if (!db) {
    alert('Database is not ready yet.');
    return;
  }

  if (sessionId === currentLogSessionId) {
    logging = false;
    currentLogSessionId = null;
    loggingButton.textContent = 'Begin Logging';
    loggingButton.classList.remove('active');
    setStatus('Stopped logging due to session removal.', false);
  }

  const transaction = db.transaction(["temperatureLogs", "logSessions"], "readwrite");
  const logsStore = transaction.objectStore("temperatureLogs");
  const sessionStore = transaction.objectStore("logSessions");
  const request = logsStore.getAll();

  request.onsuccess = (event) => {
    const logs = event.target.result.filter((record) => record.logId === sessionId);
    logs.forEach((record) => logsStore.delete(record.id));
    sessionStore.delete(sessionId);
  };

  request.onerror = (event) => {
    console.error('Failed to delete logs for session:', event.target.error);
  };

  transaction.oncomplete = () => {
    if (selectedLogId === sessionId) {
      selectedLogId = null;
    }
    loadSessions();
  };

}

function deleteAllSessions() {
  if (!confirm('Are you sure you want to delete all log sessions? This cannot be undone.')) {
    return;
  }

  if (!db) {
    alert('Database is not ready yet.');
    return;
  }

  logging = false;
  currentLogSessionId = null;
  loggingButton.textContent = 'Begin Logging';
  loggingButton.classList.remove('active');

  const transaction = db.transaction(["temperatureLogs", "logSessions"], "readwrite");
  transaction.objectStore("temperatureLogs").clear();
  transaction.objectStore("logSessions").clear();

  transaction.oncomplete = () => {
    selectedLogId = null;
    loadSessions();
    setStatus('All log sessions deleted.', false);
  };

  transaction.onerror = (event) => {
    console.error('Failed to delete all log sessions:', event.target.error);
  };
}

function downloadCSV(csvContent, filename) {
  const blob = new Blob([csvContent], { type: 'text/csv;charset=utf-8;' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.setAttribute('href', url);
  link.setAttribute('download', filename);
  link.style.visibility = 'hidden';
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
}

setTargetButton.addEventListener('click', sendTargetTemperature);
loggingButton.addEventListener('click', () => {
  logging = !logging;
  if (logging) {
    createNewLogSession();
    loggingButton.textContent = 'Stop Logging';
    loggingButton.classList.add('active');
  } else {
    closeCurrentLogSession();
    loggingButton.textContent = 'Begin Logging';
    loggingButton.classList.remove('active');
  }
});
downloadAllButton.addEventListener('click', downloadAllSessions);
deleteAllButton.addEventListener('click', deleteAllSessions);

document.addEventListener('DOMContentLoaded', () => {
  initChart();
  connectWebSocket();
  loadSessions();
});
