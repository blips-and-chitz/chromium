// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @fileoverview ARC Graphics Tracing UI.
 */

// Namespace of SVG elements
var svgNS = 'http://www.w3.org/2000/svg';

// Background color for the band with events.
var bandColor = '#d3d3d3';

// Color that should never appear on UI.
var unusedColor = '#ff0000';

/**
 * Keep in sync with ArcTracingGraphicsModel::BufferEventType
 * See chrome/browser/chromeos/arc/tracing/arc_tracing_graphics_model.h.
 * Describes how events should be rendered. |color| specifies color of the
 * event, |name| is used in tooltips.
 */
var eventAttributes = {
  // kIdleIn
  0: {color: bandColor, name: 'idle'},
  // kIdleOut
  1: {color: '#ffbf00', name: 'active'},

  // kBufferQueueDequeueStart
  100: {color: '#99cc00', name: 'app requests buffer'},
  // kBufferQueueDequeueDone
  101: {color: '#669999', name: 'app fills buffer'},
  // kBufferQueueQueueStart
  102: {color: '#cccc00', name: 'app queues buffer'},
  // kBufferQueueQueueDone
  103: {color: unusedColor, name: 'buffer is queued'},
  // kBufferQueueAcquire
  104: {color: '#66ffcc', name: 'use buffer'},
  // kBufferQueueReleased
  105: {color: unusedColor, name: 'buffer released'},
  // kBufferFillJank
  106: {color: '#ff0000', name: 'buffer filling jank', width: 1.0},

  // kExoSurfaceAttach.
  200: {color: '#99ccff', name: 'surface attach'},
  // kExoProduceResource
  201: {color: '#cc66ff', name: 'produce resource'},
  // kExoBound
  202: {color: '#66ffff', name: 'buffer bound'},
  // kExoPendingQuery
  203: {color: '#00ff99', name: 'pending query'},
  // kExoReleased
  204: {color: unusedColor, name: 'released'},
  // kExoJank
  205: {color: '#ff0000', name: 'surface attach jank', width: 1.0},

  // kChromeBarrierOrder.
  300: {color: '#ff9933', name: 'barrier order'},
  // kChromeBarrierFlush
  301: {color: unusedColor, name: 'barrier flush'},

  // kVsync
  400: {color: '#ff3300', name: 'vsync', width: 0.5},
  // kSurfaceFlingerInvalidationStart
  401: {color: '#ff9933', name: 'invalidation start'},
  // kSurfaceFlingerInvalidationDone
  402: {color: unusedColor, name: 'invalidation done'},
  // kSurfaceFlingerCompositionStart
  403: {color: '#3399ff', name: 'composition start'},
  // kSurfaceFlingerCompositionDone
  404: {color: unusedColor, name: 'composition done'},
  // kSurfaceFlingerCompositionJank
  405: {color: '#ff0000', name: 'Android composition jank', width: 1.0},

  // kChromeOSDraw
  500: {color: '#3399ff', name: 'draw'},
  // kChromeOSSwap
  501: {color: '#cc9900', name: 'swap'},
  // kChromeOSWaitForAck
  502: {color: '#ccffff', name: 'wait for ack'},
  // kChromeOSPresentationDone
  503: {color: '#ffbf00', name: 'presentation done'},
  // kChromeOSSwapDone
  504: {color: '#65f441', name: 'swap done'},
  // kChromeOSJank
  505: {color: '#ff0000', name: 'Chrome composition jank', width: 1.0},

  // Service events.
  // kTimeMark
  10000: {color: '#fff', name: 'Time mark', width: 0.75},
};

/**
 * Defines the map of events that can be treated as the end of event sequence.
 * Time after such events is considered as idle time until the next event
 * starts. Key of |endSequenceEvents| is event type as defined in
 * ArcTracingGraphicsModel::BufferEventType and value is the list of event
 * types that should follow after the tested event to consider it as end of
 * sequence. Empty list means that tested event is certainly end of the
 * sequence.
 */
var endSequenceEvents = {
  // kIdleIn
  0: [],
  // kBufferQueueQueueDone
  103: [],
  // kBufferQueueReleased
  105: [],
  // kExoReleased
  204: [],
  // kChromeBarrierFlush
  301: [],
  // kSurfaceFlingerInvalidationDone
  402: [],
  // kSurfaceFlingerCompositionDone
  404: [],
  // kChromeOSPresentationDone. Chrome does not define exactly which event
  // is the last. Different
  // pipelines may produce different sequences. Both event type may indicate
  // the end of the
  // sequence.
  503: [500 /* kChromeOSDraw */],
  // kChromeOSSwapDone
  504: [500 /* kChromeOSDraw */],
};

/**
 * @type {DetailedInfoView}.
 * Currently active detailed view.
 */
var activeDetailedInfoView = null;

/**
 * Discards active detailed view if it exists.
 */
function discardDetailedInfo() {
  if (activeDetailedInfoView) {
    activeDetailedInfoView.discard();
    activeDetailedInfoView = null;
  }
}

/**
 * Shows detailed view for |eventBand| in response to mouse click event
 * |mouseEvent|.
 */
function showDetailedInfoForBand(eventBand, mouseEvent) {
  discardDetailedInfo();
  activeDetailedInfoView = eventBand.showDetailedInfo(mouseEvent);
  mouseEvent.preventDefault();
}

/**
 * Returns text representation of timestamp in milliseconds with one number
 * after the decimal point.
 *
 * @param {number} timestamp in microseconds.
 */
function timestempToMsText(timestamp) {
  return (timestamp / 1000.0).toFixed(1);
}

/**
 * Initialises UI by setting keyboard and mouse listeners to discard detailed
 * view overlay.
 */
function initializeUi() {
  document.body.onkeydown = function(event) {
    // Escape and Enter.
    if (event.key === 'Escape' || event.key === 'Enter') {
      discardDetailedInfo();
    }
  };

  window.onclick = function(event) {
    // Detect click outside the detailed view.
    if (event.defaultPrevented || activeDetailedInfoView == null) {
      return;
    }
    if (!activeDetailedInfoView.overlay.contains(event.target)) {
      discardDetailedInfo();
    }
  };
}

/** Factory class for SVG elements. */
class SVG {
  // Creates rectangle element in the |svg| with provided attributes.
  static addRect(svg, x, y, width, height, color, opacity) {
    var rect = document.createElementNS(svgNS, 'rect');
    rect.setAttributeNS(null, 'x', x);
    rect.setAttributeNS(null, 'y', y);
    rect.setAttributeNS(null, 'width', width);
    rect.setAttributeNS(null, 'height', height);
    rect.setAttributeNS(null, 'fill', color);
    rect.setAttributeNS(null, 'stroke', 'none');
    if (opacity) {
      rect.setAttributeNS(null, 'fill-opacity', opacity);
    }
    svg.appendChild(rect);
    return rect;
  }

  // Creates line element in the |svg| with provided attributes.
  static addLine(svg, x1, y1, x2, y2, color, width) {
    var line = document.createElementNS(svgNS, 'line');
    line.setAttributeNS(null, 'x1', x1);
    line.setAttributeNS(null, 'y1', y1);
    line.setAttributeNS(null, 'x2', x2);
    line.setAttributeNS(null, 'y2', y2);
    line.setAttributeNS(null, 'stroke', color);
    line.setAttributeNS(null, 'stroke-width', width);
    svg.appendChild(line);
    return line;
  }

  // Creates circle element in the |svg| with provided attributes.
  static addCircle(svg, x, y, radius, strokeWidth, color, strokeColor) {
    var circle = document.createElementNS(svgNS, 'circle');
    circle.setAttributeNS(null, 'cx', x);
    circle.setAttributeNS(null, 'cy', y);
    circle.setAttributeNS(null, 'r', radius);
    circle.setAttributeNS(null, 'fill', color);
    circle.setAttributeNS(null, 'stroke', strokeColor);
    circle.setAttributeNS(null, 'stroke-width', strokeWidth);
    svg.appendChild(circle);
    return circle;
  }

  // Creates text element in the |svg| with provided attributes.
  static addText(svg, x, y, fontSize, textContent) {
    var text = document.createElementNS(svgNS, 'text');
    text.setAttributeNS(null, 'x', x);
    text.setAttributeNS(null, 'y', y);
    text.setAttributeNS(null, 'fill', 'black');
    text.setAttributeNS(null, 'font-size', fontSize);
    text.appendChild(document.createTextNode(textContent));
    svg.appendChild(text);
    return text;
  }
}

/**
 * Represents title for events bands that can collapse/expand controlled
 * content.
 */
class EventBandTitle {
  constructor(parent, title, className, opt_iconContent) {
    this.div = document.createElement('div');
    this.div.classList.add(className);
    if (opt_iconContent) {
      var icon = document.createElement('img');
      icon.src = 'data:image/png;base64,' + opt_iconContent;
      this.div.appendChild(icon);
    }
    var span = document.createElement('span');
    span.appendChild(document.createTextNode(title));
    this.div.appendChild(span);
    this.controlledItems = [];
    this.div.onclick = this.onClick_.bind(this);
    this.parent = parent;
    this.parent.appendChild(this.div);
  }

  /**
   * Adds extra HTML element under the control. This element will be
   * automatically expanded/collapsed together with this title.
   *
   * @param {HTMLElement} item svg element to control.
   */
  addContolledItems(item) {
    this.controlledItems.push(item);
  }

  onClick_() {
    this.div.classList.toggle('hidden');
    for (var i = 0; i < this.controlledItems.length; ++i) {
      this.controlledItems[i].classList.toggle('hidden');
    }
  }
}

/** Represents container for event bands. */
class EventBands {
  /**
   * Creates container for the event bands.
   *
   * @param {EventBandTitle} title controls visibility of this band.
   * @param {string} className class name of the svg element that represents
   *     this band. 'arc-events-top-band' is used for top-level events and
   *     'arc-events-inner-band' is used for per-buffer events.
   * @param {number} resolution the resolution of bands microseconds per 1
   *     pixel.
   * @param {number} minTimestamp the minimum timestamp to display on bands.
   * @param {number} minTimestamp the maximum timestamp to display on bands.
   */
  constructor(title, className, resolution, minTimestamp, maxTimestamp) {
    // Keep information about bands and their bounds.
    this.bands = [];
    this.globalEvents = [];
    this.resolution = resolution;
    this.minTimestamp = minTimestamp;
    this.maxTimestamp = maxTimestamp;
    this.height = 0;
    // Offset of the next band of events.
    this.nextYOffset = 0;
    this.svg = document.createElementNS(svgNS, 'svg');
    this.svg.setAttributeNS(
        'http://www.w3.org/2000/xmlns/', 'xmlns:xlink',
        'http://www.w3.org/1999/xlink');
    this.setBandOffsetX(0);
    this.setWidth(0);
    this.svg.setAttribute('height', this.height + 'px');
    this.svg.classList.add(className);

    this.setTooltip_();
    title.addContolledItems(this.svg);
    title.parent.appendChild(this.svg);
  }

  /**
   * Sets the horizontal offset to render bands.
   * @param {number} offsetX offset in pixels.
   */
  setBandOffsetX(offsetX) {
    this.bandOffsetX = offsetX;
  }

  /**
   * Sets the widths of event bands.
   * @param {number} width width in pixels.
   */
  setWidth(width) {
    this.width = width;
    this.svg.setAttribute('width', this.width + 'px');
  }

  /**
   * Converts timestamp into pixel offset. 1 pixel corresponds resolution
   * microseconds.
   *
   * @param {number} timestamp in microseconds.
   */
  timestampToOffset(timestamp) {
    return (timestamp - this.minTimestamp) / this.resolution;
  }

  /**
   * Opposite conversion of |timestampToOffset|
   *
   * @param {number} offset in pixels.
   */
  offsetToTime(offset) {
    return offset * this.resolution + this.minTimestamp;
  }

  /**
   * This adds new band of events. Height of svg container is automatically
   * adjusted to fit the new content.
   *
   * @param {Events} eventBand event band to add.
   * @param {number} height of the band.
   * @param {number} padding to separate from the next band.
   */
  addBand(eventBand, height, padding) {
    var currentColor = bandColor;
    var x = this.bandOffsetX;
    var eventIndex = eventBand.getFirstAfter(this.minTimestamp);
    while (eventIndex >= 0) {
      var event = eventBand.events[eventIndex];
      if (event[1] >= this.maxTimestamp) {
        break;
      }
      var nextX = this.timestampToOffset(event[1]) + this.bandOffsetX;
      SVG.addRect(
          this.svg, x, this.nextYOffset, nextX - x, height, currentColor);
      if (eventBand.isEndOfSequence(eventIndex)) {
        currentColor = bandColor;
      } else {
        currentColor = eventAttributes[event[0]].color;
      }
      x = nextX;
      eventIndex = eventBand.getNextEvent(eventIndex, 1 /* direction */);
    }
    SVG.addRect(
        this.svg, x, this.nextYOffset,
        this.timestampToOffset(this.maxTimestamp) - x + this.bandOffsetX,
        height, currentColor);

    this.bands.push({
      band: eventBand,
      top: this.nextYOffset,
      bottom: this.nextYOffset + height
    });

    this.nextYOffset += height;
    this.height = this.nextYOffset;
    this.svg.setAttribute('height', this.height + 'px');
    this.nextYOffset += padding;
  }

  /**
   * This adds events as a global events that do not belong to any band.
   *
   * @param {Events} events to add.
   */
  addGlobal(events) {
    var eventIndex = -1;
    while (true) {
      eventIndex = events.getNextEvent(eventIndex, 1 /* direction */);
      if (eventIndex < 0) {
        break;
      }
      var event = events.events[eventIndex];
      var attributes = events.getEventAttributes(eventIndex);
      var x = this.timestampToOffset(event[1]) + this.bandOffsetX;
      SVG.addLine(
          this.svg, x, 0, x, this.height, attributes.color, attributes.width);
    }
    this.globalEvents.push(events);
  }

  /** Initializes tooltip support by observing mouse events */
  setTooltip_() {
    this.tooltip = $('arc-event-band-tooltip');
    this.svg.onmouseover = this.showToolTip_.bind(this);
    this.svg.onmouseout = this.hideToolTip_.bind(this);
    this.svg.onmousemove = this.updateToolTip_.bind(this);
    this.svg.onclick = (event) => {
      showDetailedInfoForBand(this, event);
    };
  }

  /** Updates tooltip and shows it for this band. */
  showToolTip_(event) {
    this.updateToolTip_(event);
  }

  /** Hides the tooltip. */
  hideToolTip_() {
    this.tooltip.classList.remove('active');
  }

  /**
   * Finds the global event that is closest to the |timestamp| and not farther
   * than |distance|.
   *
   * @param {number} timestamp to search.
   * @param {number} distance to search.
   */
  findGlobalEvent_(timestamp, distance) {
    var bestDistance = distance;
    var bestEvent = null;
    for (var i = 0; i < this.globalEvents.length; ++i) {
      var globalEvents = this.globalEvents[i];
      var closestIndex = this.globalEvents[i].getClosest(timestamp);
      if (closestIndex >= 0) {
        var testEvent = this.globalEvents[i].events[closestIndex];
        var testDistance = Math.abs(testEvent[1] - timestamp);
        if (testDistance < bestDistance) {
          bestDistance = testDistance;
          bestEvent = testEvent;
        }
      }
    }
    return bestEvent;
  }

  /**
   * Updates tool tip based on event under the current cursor.
   *
   * @param {Object} event mouse event.
   */
  updateToolTip_(event) {
    // Clear previous content.
    this.tooltip.textContent = '';

    // Tooltip constants for rendering.
    var horizontalGap = 10;
    var eventIconOffset = 70;
    var eventIconRadius = 4;
    var eventNameOffset = 78;
    var verticalGap = 5;
    var lineHeight = 16;
    var fontSize = 12;

    var offsetX = event.offsetX - this.bandOffsetX;
    if (offsetX < 0) {
      this.tooltip.classList.remove('active');
      return;
    }

    // Find band for this mouse event.
    var eventBand = undefined;

    for (var i = 0; i < this.bands.length; ++i) {
      if (this.bands[i].top <= event.offsetY &&
          this.bands[i].bottom > event.offsetY) {
        eventBand = this.bands[i].band;
        break;
      }
    }

    if (!eventBand) {
      this.tooltip.classList.remove('active');
      return;
    }

    var svg = document.createElementNS(svgNS, 'svg');
    svg.setAttributeNS(
        'http://www.w3.org/2000/xmlns/', 'xmlns:xlink',
        'http://www.w3.org/1999/xlink');
    this.tooltip.appendChild(svg);

    var yOffset = verticalGap + lineHeight;
    var eventTimestamp = this.offsetToTime(offsetX);
    SVG.addText(
        svg, horizontalGap, yOffset, fontSize,
        timestempToMsText(eventTimestamp) + ' ms');
    yOffset += lineHeight;

    // Find the event under the cursor. |index| points to the current event
    // and |nextIndex| points to the next event.
    var nextIndex = eventBand.getNextEvent(-1 /* index */, 1 /* direction */);
    while (nextIndex >= 0) {
      if (eventBand.events[nextIndex][1] > eventTimestamp) {
        break;
      }
      nextIndex = eventBand.getNextEvent(nextIndex, 1 /* direction */);
    }
    var index = eventBand.getNextEvent(nextIndex, -1 /* direction */);

    // Try to find closest global event in the range -200..200 mcs from
    // |eventTimestamp|.
    var globalEvent = this.findGlobalEvent_(eventTimestamp, 200 /* distance */);
    if (globalEvent) {
      // Show the global event info.
      var attributes = eventAttributes[globalEvent[0]];
      SVG.addText(
          svg, horizontalGap, yOffset, 12,
          attributes.name + ' ' + timestempToMsText(globalEvent[1]) + ' ms.');
    } else if (index < 0 || eventBand.isEndOfSequence(index)) {
      // In case cursor points to idle event, show its interval.
      var startIdle = index < 0 ? 0 : eventBand.events[index][1];
      var endIdle =
          nextIndex < 0 ? this.maxTimestamp : eventBand.events[nextIndex][1];
      SVG.addText(
          svg, horizontalGap, yOffset, 12,
          'Idle ' + timestempToMsText(startIdle) + '...' +
              timestempToMsText(endIdle) + ' ms.');
      yOffset += lineHeight;
    } else {
      // Show the sequence of non-idle events.
      // Find the start of the non-idle sequence.
      while (true) {
        var prevIndex = eventBand.getNextEvent(index, -1 /* direction */);
        if (prevIndex < 0 || eventBand.isEndOfSequence(prevIndex)) {
          break;
        }
        index = prevIndex;
      }

      var sequenceTimestamp = eventBand.events[index][1];
      var lastTimestamp = sequenceTimestamp;
      var firstEvent = true;
      // Scan for the entries to show.
      var entriesToShow = [];
      while (index >= 0) {
        var attributes = eventBand.getEventAttributes(index);
        var eventTimestamp = eventBand.events[index][1];
        var entryToShow = {};
        if (firstEvent) {
          // Show the global timestamp.
          entryToShow.prefix = timestempToMsText(sequenceTimestamp) + ' ms';
          firstEvent = false;
        } else {
          // Show the offset relative to the start of sequence of events.
          entryToShow.prefix = '+' +
              timestempToMsText(eventTimestamp - sequenceTimestamp) + ' ms';
        }
        entryToShow.color = attributes.color;
        entryToShow.text = attributes.name;
        if (entriesToShow.length > 0) {
          entriesToShow[entriesToShow.length - 1].text +=
              ' [' + timestempToMsText(eventTimestamp - lastTimestamp) + ' ms]';
        }
        entriesToShow.push(entryToShow);
        if (eventBand.isEndOfSequence(index)) {
          break;
        }
        lastTimestamp = eventTimestamp;
        index = eventBand.getNextEvent(index, 1 /* direction */);
      }

      // Last element is end of sequence, use bandColor for the icon.
      if (entriesToShow.length > 0) {
        entriesToShow[entriesToShow.length - 1].color = bandColor;
      }
      for (var i = 0; i < entriesToShow.length; ++i) {
        var entryToShow = entriesToShow[i];
        SVG.addText(svg, horizontalGap, yOffset, fontSize, entryToShow.prefix);
        SVG.addCircle(
            svg, eventIconOffset, yOffset - eventIconRadius, eventIconRadius, 1,
            entryToShow.color, 'black');
        SVG.addText(svg, eventNameOffset, yOffset, fontSize, entryToShow.text);
        yOffset += lineHeight;
      }
    }
    if (this.canShowDetailedInfo()) {
      SVG.addText(
          svg, horizontalGap, yOffset, fontSize, 'Click for detailed info');
      yOffset += lineHeight;
    }
    yOffset += verticalGap;

    this.tooltip.style.left = event.clientX + 'px';
    this.tooltip.style.top = event.clientY + 'px';
    this.tooltip.style.height = yOffset + 'px';
    this.tooltip.classList.add('active');
  }

  /**
   * Returns true in case band can show detailed info.
   */
  canShowDetailedInfo() {
    return false;
  }

  /**
   * Shows detailed info for the position under mouse event |event|. By default
   * it creates nothing.
   */
  showDetailedInfo(event) {
    return null;
  }
}

/**
 * Base class for detailed info view.
 */
class DetailedInfoView {
  discard() {}
}

/**
 * CPU detailed info view. Renders 4x zoomed CPU events split by processes and
 * threads.
 */
class CpuDetailedInfoView extends DetailedInfoView {
  create(overviewBand) {
    this.overlay = $('arc-detailed-view-overlay');
    var overviewRect = overviewBand.svg.getBoundingClientRect();

    // Clear previous content.
    this.overlay.textContent = '';

    // UI constants to render.
    var columnWidth = 140;
    var scrollBarWidth = 3;
    var zoomFactor = 4.0;
    var cpuBandHeight = 14;
    var processInfoHeight = 14;
    var padding = 2;
    var fontSize = 12;
    var processInfoPadding = 2;
    var threadInfoPadding = 6;

    // Use minimum 80% of inner width or 600 pixels to display detailed view
    // zoomed |zoomFactor| times.
    var availableWidthPixels =
        window.innerWidth * 0.8 - columnWidth - scrollBarWidth;
    availableWidthPixels = Math.max(availableWidthPixels, 600);
    var availableForHalfBandMcs = Math.floor(
        overviewBand.offsetToTime(availableWidthPixels) / (2.0 * zoomFactor));
    // Determine the interval to display.
    var eventTimestamp =
        overviewBand.offsetToTime(event.offsetX - overviewBand.bandOffsetX);
    var minTimestamp = eventTimestamp - availableForHalfBandMcs;
    var maxTimestamp = eventTimestamp + availableForHalfBandMcs + 1;
    var duration = maxTimestamp - minTimestamp;

    // Construct sub-model of active/idle events per each thread, active within
    // this interval.
    var eventsPerTid = {};
    for (var cpuId = 0; cpuId < overviewBand.model.cpu.events.length; cpuId++) {
      var activeEvents = new Events(
          overviewBand.model.cpu.events[cpuId], 3 /* kActive */,
          3 /* kActive */);
      var activeTid = 0;
      var index = activeEvents.getFirstAfter(minTimestamp);
      var activeStartTimestamp = minTimestamp;
      while (index >= 0 && activeEvents.events[index][1] < maxTimestamp) {
        this.addActivityTime_(
            eventsPerTid, activeTid, activeStartTimestamp,
            activeEvents.events[index][1]);
        activeTid = activeEvents.events[index][2];
        activeStartTimestamp = activeEvents.events[index][1];
        index = activeEvents.getNextEvent(index, 1 /* direction */);
      }
      this.addActivityTime_(
          eventsPerTid, activeTid, activeStartTimestamp, maxTimestamp - 1);
    }

    // The same thread might be executed on different CPU cores. Sort events.
    for (var tid in eventsPerTid) {
      eventsPerTid[tid].events.sort(function(a, b) {
        return a[1] - b[1];
      });
    }

    // Group threads by process.
    var threadsPerPid = {};
    var pids = [];
    var totalTime = 0;
    for (var tid in eventsPerTid) {
      var thread = eventsPerTid[tid];
      var pid = overviewBand.model.cpu.threads[tid].pid;
      if (!(pid in threadsPerPid)) {
        pids.push(pid);
        threadsPerPid[pid] = {};
        threadsPerPid[pid].totalTime = 0;
        threadsPerPid[pid].threads = [];
      }
      threadsPerPid[pid].totalTime += thread.totalTime;
      threadsPerPid[pid].threads.push(thread);
      totalTime += thread.totalTime;
    }

    // Sort processes per time usage.
    pids.sort(function(a, b) {
      return threadsPerPid[b].totalTime - threadsPerPid[a].totalTime;
    });

    var totalUsage = 100.0 * totalTime / duration;
    var cpuInfo = 'CPU view. ' + pids.length + '/' +
        Object.keys(eventsPerTid).length +
        ' active processes/threads. Total cpu usage: ' + totalUsage.toFixed(2) +
        '%.';
    var title = new EventBandTitle(this.overlay, cpuInfo, 'arc-cpu-view-title');
    var bands = new EventBands(
        title, 'arc-events-cpu-detailed-band',
        overviewBand.resolution / zoomFactor, minTimestamp, maxTimestamp);
    bands.setBandOffsetX(columnWidth);
    var bandsWidth = bands.timestampToOffset(maxTimestamp);
    var totalWidth = bandsWidth + columnWidth;
    bands.setWidth(totalWidth);

    for (i = 0; i < pids.length; i++) {
      var pid = pids[i];
      var threads = threadsPerPid[pid].threads;
      var processName;
      if (pid in overviewBand.model.cpu.threads) {
        processName = overviewBand.model.cpu.threads[pid].name;
      } else {
        processName = 'Others';
      }
      bands.nextYOffset += (processInfoHeight + padding);
      var processCpuUsage = 100.0 * threadsPerPid[pid].totalTime / duration;
      var processInfo = processName + ' <' + pid +
          '>, cpu usage: ' + processCpuUsage.toFixed(2) + '%.';
      SVG.addText(
          bands.svg, processInfoPadding, bands.nextYOffset - 2 * padding,
          fontSize, processInfo);
      bands.svg.setAttribute('height', bands.nextYOffset + 'px');

      // Sort threads per time usage.
      threads.sort(function(a, b) {
        return eventsPerTid[b.tid].totalTime - eventsPerTid[a.tid].totalTime;
      });

      for (j = 0; j < threads.length; j++) {
        var tid = threads[j].tid;
        bands.addBand(
            new Events(eventsPerTid[tid].events, 0, 1), cpuBandHeight, padding);
        var threadName = overviewBand.model.cpu.threads[tid].name;
        var threadCpuUsage = 100.0 * threads[j].totalTime / duration;
        var threadInfo = threadName + ' ' + threadCpuUsage.toFixed(2) + '%';
        SVG.addText(
            bands.svg, threadInfoPadding, bands.nextYOffset - padding, fontSize,
            threadInfo);
      }
    }

    // Add center and boundary lines.
    var kTimeMark = 10000;
    var timeEvents = [
      [kTimeMark, minTimestamp], [kTimeMark, eventTimestamp],
      [kTimeMark, maxTimestamp - 1]
    ];
    bands.addGlobal(new Events(timeEvents, kTimeMark, kTimeMark));

    // Mark zoomed interval in overview.
    var overviewX = overviewBand.timestampToOffset(minTimestamp);
    var overviewWidth =
        overviewBand.timestampToOffset(maxTimestamp) - overviewX;
    this.bandSelection = SVG.addRect(
        overviewBand.svg, overviewX, 0, overviewWidth, overviewBand.height,
        '#000' /* color */, 0.1 /* opacity */);

    // Align position in overview and middle line here if possible.
    var left = Math.max(
        Math.min(
            Math.round(event.clientX - columnWidth - bandsWidth * 0.5),
            window.innerWidth - totalWidth),
        0);
    this.overlay.style.left = left + 'px';
    // Place below the overview with small gap.
    this.overlay.style.top = (overviewRect.bottom + window.scrollY + 2) + 'px';
    this.overlay.classList.add('active');
  }

  discard() {
    this.overlay.classList.remove('active');
    this.bandSelection.remove();
  }

  /**
   * Helper that adds kIdleIn/kIdle events into the dictionary.
   *
   * @param {Object} eventsPerTid dictionary to fill. Key is thread id and
   *     value is object that contains all events for thread with related
   *     information.
   * @param {number} tid thread id.
   * @param {number} timestampFrom start time of thread activity.
   * @param {number} timestampTo end time of thread activity.
   */
  addActivityTime_(eventsPerTid, tid, timestampFrom, timestampTo) {
    if (tid == 0) {
      // Don't process idle thread.
      return;
    }
    if (!(tid in eventsPerTid)) {
      // Create the band for the new thread.
      eventsPerTid[tid] = {};
      eventsPerTid[tid].totalTime = 0;
      eventsPerTid[tid].events = [];
      eventsPerTid[tid].tid = tid;
    }
    eventsPerTid[tid].events.push([1 /* kIdleOut */, timestampFrom]);
    eventsPerTid[tid].events.push([0 /* kIdleIn */, timestampTo]);
    // Update total time for this thread.
    eventsPerTid[tid].totalTime += (timestampTo - timestampFrom);
  }
}

class CpuEventBands extends EventBands {
  setModel(model) {
    this.model = model;
    var bandHeight = 6;
    var padding = 2;
    for (var cpuId = 0; cpuId < this.model.cpu.events.length; cpuId++) {
      this.addBand(
          new Events(this.model.cpu.events[cpuId], 0, 1), bandHeight, padding);
    }
  }

  canShowDetailedInfo() {
    return true;
  }

  showDetailedInfo(event) {
    var view = new CpuDetailedInfoView();
    view.create(this);
    return view;
  }
}

/** Represents one band with events. */
class Events {
  /**
   * Assigns events for this band. Events with type between |eventTypeMin| and
   * |eventTypeMax| are only displayed on the band.
   *
   * @param {Object[]} events non-filtered list of all events. Each has array
   *     where first element is type and second is timestamp.
   * @param {number} eventTypeMin minimum inclusive type of the event to be
   *     displayed on this band.
   * @param {number} eventTypeMax maximum inclusive type of the event to be
   *     displayed on this band.
   */
  constructor(events, eventTypeMin, eventTypeMax) {
    this.events = events;
    this.eventTypeMin = eventTypeMin;
    this.eventTypeMax = eventTypeMax;
  }

  /**
   * Helper that finds next or previous event. Events that pass filter are
   * only processed.
   *
   * @param {number} index starting index for the search, not inclusive.
   * @param {direction} direction to search, 1 means to find the next event and
   *     -1 means the previous event.
   * @returns {number} index of the next or previous event or -1 in case not
   *     found.
   */
  getNextEvent(index, direction) {
    while (true) {
      index += direction;
      if (index < 0 || index >= this.events.length) {
        return -1;
      }
      if (this.events[index][0] >= this.eventTypeMin &&
          this.events[index][0] <= this.eventTypeMax) {
        return index;
      }
    }
  }

  /**
   * Helper that returns render attributes for the event.
   *
   * @param {number} index element index in |this.events|.
   */
  getEventAttributes(index) {
    return eventAttributes[this.events[index][0]];
  }

  /**
   * Returns true if the tested event denotes end of event sequence.
   *
   * @param {number} index element index in |this.events|.
   */
  isEndOfSequence(index) {
    var nextEventTypes = endSequenceEvents[this.events[index][0]];
    if (!nextEventTypes) {
      return false;
    }
    if (nextEventTypes.length == 0) {
      return true;
    }
    var nextIndex = this.getNextEvent(index, 1 /* direction */);
    if (nextIndex < 0) {
      // No more events after and it is listed as possible end of sequence.
      return true;
    }
    return nextEventTypes.includes(this.events[nextIndex][0]);
  }

  /**
   * Returns the index of closest event to the requested |timestamp|.
   *
   * @param {number} timestamp to search.
   */
  getClosest(timestamp) {
    if (this.events.length == 0) {
      return -1;
    }
    if (this.events[0][1] >= timestamp) {
      return this.getNextEvent(-1 /* index */, 1 /* direction */);
    }
    if (this.events[this.events.length - 1][1] <= timestamp) {
      return this.getNextEvent(
          this.events.length /* index */, -1 /* direction */);
    }
    // At this moment |firstBefore| and |firstAfter| points to any event.
    var firstBefore = 0;
    var firstAfter = this.events.length - 1;
    while (firstBefore + 1 != firstAfter) {
      var candidateIndex = Math.ceil((firstBefore + firstAfter) / 2);
      if (this.events[candidateIndex][1] < timestamp) {
        firstBefore = candidateIndex;
      } else {
        firstAfter = candidateIndex;
      }
    }
    // Point |firstBefore| and |firstAfter| to the supported event types.
    firstBefore =
        this.getNextEvent(firstBefore + 1 /* index */, -1 /* direction */);
    firstAfter =
        this.getNextEvent(firstAfter - 1 /* index */, 1 /* direction */);
    if (firstBefore < 0) {
      return firstAfter;
    } else if (firstAfter < 0) {
      return firstBefore;
    } else {
      var diffBefore = timestamp - this.events[firstBefore][1];
      var diffAfter = this.events[firstAfter][1] - timestamp;
      if (diffBefore < diffAfter) {
        return firstBefore;
      } else {
        return firstAfter;
      }
    }
  }

  /**
   * Returns the index of the first event after or on requested |timestamp|.
   *
   * @param {number} timestamp to search.
   */
  getFirstAfter(timestamp) {
    var closest = this.getClosest(timestamp);
    if (closest < 0) {
      return -1;
    }
    if (this.events[closest][1] >= timestamp) {
      return closest;
    }
    return this.getNextEvent(closest, 1 /* direction */);
  }
}

/**
 * Creates visual representation of graphic buffers event model.
 *
 * @param {Object} model object produced by |ArcTracingGraphicsModel|.
 */
function setGraphicBuffersModel(model) {
  // Clear previous content.
  $('arc-event-bands').textContent = '';

  // Microseconds per pixel.
  var resolution = 100.0;
  var parent = $('arc-event-bands');

  var topBandHeight = 16;
  var topBandPadding = 4;
  var innerBandHeight = 12;
  var innerBandPadding = 2;
  var innerLastBandPadding = 12;

  var cpusTitle = new EventBandTitle(parent, 'CPUs', 'arc-events-band-title');
  var cpusBands = new CpuEventBands(
      cpusTitle, 'arc-events-band', resolution, 0, model.duration);
  cpusBands.setWidth(cpusBands.timestampToOffset(model.duration));
  cpusBands.setModel(model);

  var vsyncEvents = new Events(
      model.android.global_events, 400 /* kVsync */, 400 /* kVsync */);

  var chromeTitle =
      new EventBandTitle(parent, 'Chrome graphics', 'arc-events-band-title');
  var chromeBands = new EventBands(
      chromeTitle, 'arc-events-band', resolution, 0, model.duration);
  chromeBands.setWidth(chromeBands.timestampToOffset(model.duration));
  for (i = 0; i < model.chrome.buffers.length; i++) {
    chromeBands.addBand(
        new Events(model.chrome.buffers[i], 500, 599), topBandHeight,
        topBandPadding);
  }

  chromeBands.addGlobal(new Events(
      model.chrome.global_events, 505 /* kChromeOSJank */,
      505 /* kChromeOSJank */));

  var androidTitle =
      new EventBandTitle(parent, 'Android graphics', 'arc-events-band-title');
  var androidBands = new EventBands(
      androidTitle, 'arc-events-band', resolution, 0, model.duration);
  androidBands.setWidth(androidBands.timestampToOffset(model.duration));
  androidBands.addBand(
      new Events(model.android.buffers[0], 400, 499), topBandHeight,
      topBandPadding);
  // Add vsync events
  androidBands.addGlobal(vsyncEvents);
  androidBands.addGlobal(new Events(
      model.android.global_events, 405 /* kSurfaceFlingerCompositionJank */,
      405 /* kSurfaceFlingerCompositionJank */));

  for (i = 0; i < model.views.length; i++) {
    var view = model.views[i];
    var activityTitleText;
    var icon;
    if (model.tasks && view.task_id in model.tasks) {
      activityTitleText =
          model.tasks[view.task_id].title + ' - ' + view.activity;
      icon = model.tasks[view.task_id].icon;
    } else {
      activityTitleText = 'Task #' + view.task_id + ' - ' + view.activity;
    }
    var activityTitle = new EventBandTitle(
        parent, activityTitleText, 'arc-events-band-title', icon);
    var activityBands = new EventBands(
        activityTitle, 'arc-events-band', resolution, 0, model.duration);
    activityBands.setWidth(activityBands.timestampToOffset(model.duration));
    for (j = 0; j < view.buffers.length; j++) {
      var androidBand =
          new Events(activityTitle, 'arc-events-band', model.duration, 14);
      // Android buffer events.
      activityBands.addBand(
          new Events(view.buffers[j], 100, 199), innerBandHeight,
          innerBandPadding);
      // exo events.
      activityBands.addBand(
          new Events(view.buffers[j], 200, 299), innerBandHeight,
          innerLastBandPadding);
      // Chrome buffer events are not displayed at this time.
    }
    // Add vsync events
    activityBands.addGlobal(vsyncEvents);
    activityBands.addGlobal(new Events(
        view.global_events, 106 /* kBufferFillJank */,
        106 /* kBufferFillJank */));
  }
}
