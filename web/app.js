
// The start of the page and the helpers that the views share. The answers of
// the board are kept in $d, where the templates read them.

/* global El, LiteJS, xhr */

// The same names as a %js block of a view file gets.
var $ui = LiteJS({ home: "fans" })
, $d = $ui.$d
, $ = $ui.$

$d.wifi = {}
$d.vpn = {}
$d.ota = {}

// The menu shows which view is open.
$ui.on("show", draw)

// Refresh reads the open view again, the way opening it does.
$ui.on("refresh", function() {
	$ui($ui.route).emit("ping")
})

// Nothing is drawn again by itself: change the data, then draw().
function draw() {
	El.render(document.body)
}

// A list in a template has to stay the same array, so it is filled, not
// replaced.
function fill(arr, list) {
	arr.length = 0
	arr.push.apply(arr, list)
}

function say(text) {
	$d.log = text
	draw()
}

// Ask for /api/<key>, keep the answer as $d.<key> and pass it on. An answer
// that is not JSON counts as no answer. The time in the address keeps the
// browser from answering out of its cache.
function get(key, next) {
	xhr("GET", "/api/" + key + "?" + Date.now(), function(err, text) {
		var data = {}
		try {
			if (!err) data = JSON.parse(text)
		} catch (e) {
			err = 1
		}
		next($d[key] = data, err)
	}).send()
}

// Send a change and show it with the answer in the log line. Of the body
// only the first line is shown, or the text given in its place.
function send(method, key, body, next, shown) {
	var line = method + " /api/" + key + "  " + (shown || body.split("\n")[0])
	say(line)
	xhr(method, "/api/" + key, function(err, text) {
		say(line + "\n" + (err === 1 ? "the board does not answer" : this.status + "  " + text))
		next(err ? null : text)
	}).send(body)
}
