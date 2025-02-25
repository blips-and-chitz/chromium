# Copyright 2017 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
# """Model objects for ukm.xml contents."""

import os
import sys

sys.path.append(os.path.join(os.path.dirname(__file__), '..', 'common'))
import models

# Model definitions for ukm.xml content
_OBSOLETE_TYPE = models.TextNodeType('obsolete')
_OWNER_TYPE = models.TextNodeType('owner', single_line=True)
_SUMMARY_TYPE = models.TextNodeType('summary')

_LOWERCASE_NAME_FN = lambda n: n.attributes['name'].value.lower()

_ENUMERATION_TYPE = models.ObjectNodeType(
    'enumeration',
    attributes=[
      ('export', unicode, r'^(?i)(|true|false)$'),
    ],
    single_line=True)

_QUANTILES_TYPE = models.ObjectNodeType(
    'quantiles',
    attributes=[
      ('type', unicode, None),
      ('export', unicode, r'^(?i)(|true|false)$'),
    ],
    single_line=True)

_INDEX_TYPE = models.ObjectNodeType(
    'index',
    attributes=[
      ('fields', unicode, None),
    ],
    single_line=True)

_STATISTICS_TYPE =  models.ObjectNodeType(
    'statistics',
    attributes=[],
    children=[
        models.ChildType('quantiles', _QUANTILES_TYPE, multiple=False),
        models.ChildType('enumeration', _ENUMERATION_TYPE, multiple=False),
    ])

_HISTORY_TYPE =  models.ObjectNodeType(
    'history',
    attributes=[],
    children=[
        models.ChildType('index', _INDEX_TYPE, multiple=True),
        models.ChildType('statistics', _STATISTICS_TYPE, multiple=True),
    ])

_AGGREGATION_TYPE =  models.ObjectNodeType(
    'aggregation',
    attributes=[],
    children=[
        models.ChildType('history', _HISTORY_TYPE, multiple=False),
    ])

_METRIC_TYPE =  models.ObjectNodeType(
    'metric',
    attributes=[
      ('name', unicode, r'^[A-Za-z0-9_.]+$'),
      ('semantic_type', unicode, None),
    ],
    children=[
        models.ChildType('obsolete', _OBSOLETE_TYPE, multiple=False),
        models.ChildType('owners', _OWNER_TYPE, multiple=True),
        models.ChildType('summary', _SUMMARY_TYPE, multiple=False),
        models.ChildType('aggregation', _AGGREGATION_TYPE, multiple=True),
    ])

_EVENT_TYPE =  models.ObjectNodeType(
    'event',
    alphabetization=[('metric', _LOWERCASE_NAME_FN)],
    attributes=[
      ('name', unicode, r'^[A-Za-z0-9.]+$'),
      ('singular', unicode, r'^(?i)(|true|false)$'),
    ],
    extra_newlines=(1, 1, 1),
    children=[
        models.ChildType('obsolete', _OBSOLETE_TYPE, multiple=False),
        models.ChildType('owners', _OWNER_TYPE, multiple=True),
        models.ChildType('summary', _SUMMARY_TYPE, multiple=False),
        models.ChildType('metrics', _METRIC_TYPE, multiple=True),
    ])

_UKM_CONFIGURATION_TYPE = models.ObjectNodeType(
    'ukm-configuration',
    extra_newlines=(2, 1, 1),
    indent=False,
    children=[
        models.ChildType('events', _EVENT_TYPE, multiple=True),
    ])

UKM_XML_TYPE = models.DocumentType(_UKM_CONFIGURATION_TYPE)

def UpdateXML(original_xml):
  """Parses the original xml and return a pretty printed version.

  Args:
    original_xml: A string containing the original xml file contents.

  Returns:
    A pretty-printed xml string, or None if the config contains errors.
  """
  config = UKM_XML_TYPE.Parse(original_xml)

  return UKM_XML_TYPE.PrettyPrint(config)
