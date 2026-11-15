// Phone-side GPS tracking. The watch sends CMD (start/pause/resume/stop);
// we reply with cumulative DIST (decimeters) and GPS (1 = have fix).
var Clay = require("@rebble/clay");
var clay = new Clay(require("./config"));

var CMD_STOP = 0, CMD_START = 1, CMD_PAUSE = 2, CMD_RESUME = 3;

var watchId = null;
var counting = false;
var last = null;
var totalMeters = 0;
var hasFix = 0;

function toRad(d) { return d * Math.PI / 180; }

function haversine(a, b) {
  var R = 6371000;
  var dLat = toRad(b.latitude - a.latitude);
  var dLon = toRad(b.longitude - a.longitude);
  var s = Math.sin(dLat / 2) * Math.sin(dLat / 2) +
          Math.cos(toRad(a.latitude)) * Math.cos(toRad(b.latitude)) *
          Math.sin(dLon / 2) * Math.sin(dLon / 2);
  return 2 * R * Math.atan2(Math.sqrt(s), Math.sqrt(1 - s));
}

function send(fix) {
  Pebble.sendAppMessage({ DIST: Math.round(totalMeters * 10), GPS: fix },
    function () {}, function () {});
}

function onPosition(pos) {
  var c = pos.coords;
  var good = c.accuracy <= 40;
  if (good !== !!hasFix) {
    hasFix = good ? 1 : 0;
    send(hasFix);
  }
  if (!good || !counting) { return; }
  if (last) {
    var d = haversine(last, c);
    // Ignore GPS jitter while standing still; accept real movement.
    if (d >= Math.max(3, c.accuracy / 4)) {
      totalMeters += d;
      last = { latitude: c.latitude, longitude: c.longitude };
      send(1);
    }
  } else {
    last = { latitude: c.latitude, longitude: c.longitude };
  }
}

function onError(err) {
  console.log('geolocation error: ' + err.message);
  if (hasFix) { hasFix = 0; send(0); }
}

function startWatching() {
  if (watchId !== null) { return; }
  watchId = navigator.geolocation.watchPosition(onPosition, onError,
    { enableHighAccuracy: true, maximumAge: 1000, timeout: 20000 });
}

function stopWatching() {
  if (watchId !== null) {
    navigator.geolocation.clearWatch(watchId);
    watchId = null;
  }
  counting = false;
  last = null;
  hasFix = 0;
}

Pebble.addEventListener('appmessage', function (e) {
  var cmd = e.payload.CMD;
  if (cmd === undefined) { return; }
  if (cmd === CMD_START) {
    totalMeters = 0;
    last = null;
    counting = true;
    startWatching();
    send(hasFix);
  } else if (cmd === CMD_PAUSE) {
    counting = false;
    last = null;
  } else if (cmd === CMD_RESUME) {
    last = null;
    counting = true;
  } else if (cmd === CMD_STOP) {
    stopWatching();
  }
});
