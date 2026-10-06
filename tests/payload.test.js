const assert = require('assert');
const fs = require('fs');
const path = require('path');

global.window = {};
global.MessagePort = function () {};
global.MessagePort.prototype = { addEventListener() {} };
global.requestAnimationFrame = () => 0;
global.localStorage = Object.defineProperties({}, {
  setItem: { value(k, v) { this[k] = String(v); } },
  getItem: { value(k) { return Object.prototype.hasOwnProperty.call(this, k) ? this[k] : null; } },
  removeItem: { value(k) { delete this[k]; } }
});
global.document = {
  documentElement: {},
  addEventListener() {},
  querySelector: () => null,
  querySelectorAll: () => []
};
global.MutationObserver = function () { return { observe() {} }; };
global.URL = require('url').URL;

const file = process.argv[2] || path.join(__dirname, '..', 'src', 'payload', 'inject.js');
eval(fs.readFileSync(file, 'utf8'));
const t = window.__outlookMeetJoinTest;

assert.strictEqual(t.normalize('meet.google.com/abc-defg-hij'), 'https://meet.google.com/abc-defg-hij');
assert.strictEqual(t.normalize('https://meet.google.com/ABC-DEFG-HIJ?x=1'), 'https://meet.google.com/abc-defg-hij');
assert.strictEqual(t.normalize('https://zoom.us/j/123'), null);
assert.ok(t.isTel('https://tel.meet/abc-defg-hij?pin=1'));
assert.ok(!t.isTel('https://meet.google.com/abc-defg-hij'));

const body = 'Google Meet meeting<br><a href="https://meet.google.com/qwe-rtyu-iop">meet.google.com/qwe-rtyu-iop</a><br>Link type:more<br><a href="https://tel.meet/qwe-rtyu-iop?pin=0">x</a>';
assert.strictEqual(t.extractFromHtml(body), 'https://meet.google.com/qwe-rtyu-iop');
assert.strictEqual(t.extractFromHtml('<a href="https://tel.meet/abc-defg-hij?pin=1">dial</a>'), null);

const ev = t.parseEvent({
  Subject: 'Weekly planning',
  Body: { BodyType: 'HTML', Value: body },
  ItemId: { Id: 'AAkITEM1', mailboxInfo: { mailboxSmtpAddress: 'work@example.com' } },
  SeriesMasterItemId: { Id: 'SERIES1' },
  UID: 'UID1'
});
assert.strictEqual(ev.link, 'https://meet.google.com/qwe-rtyu-iop');
assert.strictEqual(ev.email, 'work@example.com');
assert.deepStrictEqual(ev.keys, ['AAkITEM1', 'SERIES1', 'UID1', 'subj:weekly planning']);
assert.strictEqual(t.parseEvent({ Subject: 'No meet', Body: { Value: 'zoom.us/j/1' } }), null);

t.scanStream(JSON.stringify({ results: [
  { __typename: 'ClientCalendarEvent', Subject: 'Alpha', ItemId: { Id: 'AAkALPHA', mailboxInfo: { mailboxSmtpAddress: 'work@example.com' } }, Body: { BodyType: 'HTML', Value: '<a href="https://meet.google.com/aaa-aaaa-aaa">a</a>' } },
  { __typename: 'ClientCalendarEvent', Subject: 'Beta', ItemId: { Id: 'AAkBETA', mailboxInfo: { mailboxSmtpAddress: 'personal@example.com' } }, Body: { BodyType: 'HTML', Value: '<a href="https://meet.google.com/bbb-bbbb-bbb">b</a>' } }
] }));
assert.strictEqual(t.load('AAkALPHA').link, 'https://meet.google.com/aaa-aaaa-aaa', 'each event keeps its own link');
assert.strictEqual(t.load('AAkBETA').link, 'https://meet.google.com/bbb-bbbb-bbb', 'each event keeps its own link');
assert.strictEqual(t.load('subj:beta').email, 'personal@example.com');

t.scanStream({ nested: { deep: [{ Subject: 'Object form', UID: 'UIDOBJ', Body: { Value: 'meet.google.com/ccc-cccc-ccc' } }] } });
assert.strictEqual(t.load('UIDOBJ').link, 'https://meet.google.com/ccc-cccc-ccc', 'structured message data');

assert.strictEqual(t.emailForLink('https://meet.google.com/bbb-bbbb-bbb'), 'personal@example.com', 'email of that meeting, not the latest one');
assert.strictEqual(t.emailForLink('https://meet.google.com/aaa-aaaa-aaa'), 'work@example.com');

assert.strictEqual(
  t.buildJoinUrl('https://meet.google.com/qwe-rtyu-iop', 'work@example.com'),
  'https://meet.google.com/qwe-rtyu-iop?authuser=work%40example.com'
);
assert.strictEqual(t.buildJoinUrl('https://meet.google.com/x-y-z', null), 'https://meet.google.com/x-y-z');

t.scanStream(JSON.stringify({ Subject: 'Sync', UID: 'UIDSYNC', Body: { Value: 'meet.google.com/sss-ssss-sss' } }));
assert.strictEqual(t.byCardLines('Calendar - work@example.com\nSync\nWed 10/7/2026 10:00 - 10:15').link, 'https://meet.google.com/sss-ssss-sss');
assert.strictEqual(t.byCardLines('Calendar - work@example.com\nBackend sync review\nWed 10/7/2026'), null, 'no partial subject match');
assert.strictEqual(t.byTileLabel('Sync, 10:00 to 10:15, Wednesday').link, 'https://meet.google.com/sss-ssss-sss');
assert.strictEqual(t.byTileLabel('Sync extended, 11:00'), null);

t.scanStream('plain text without meetings');
t.scanStream('meet.google.com but not json');
t.scanStream({ junk: true });

for (let i = 0; i < 500; i++) localStorage.setItem('omjv1:bulk' + i, JSON.stringify({ link: 'x', t: i }));
t.prune();
const left = Object.keys(localStorage).filter(k => k.startsWith('omjv1:'));
assert.ok(left.length <= 400, 'cache is capped');
assert.ok(!left.includes('omjv1:bulk0') && left.includes('omjv1:bulk499'), 'oldest entries go first');

console.log('ok: all payload checks passed');
