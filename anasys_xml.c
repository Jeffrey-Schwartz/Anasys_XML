/*
 *  $Id: anasys_xml.c 22442 2019-08-27 12:41:00Z yeti-dn $
 *  Copyright (C) 2018 Jeffrey J. Schwartz.
 *  E-mail: schwartz@physics.ucla.edu
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 */

/*
 * This module serves to open Anasys Instruments / Analysis Studio
 * XML data files in Gywddion.
 * Multiple data channels (HeightMaps) are supported with meta data
 * and spectra (RenderedSpectra) import.  No file export is supported;
 * it is assumed that no changes will be saved, or if so then another
 * file format will be used.
 */

/**
 * [FILE-MAGIC-USERGUIDE]
 * Analysis Studio XML
 * .axd, .axz
 * Read SPS
 **/

/**
 * [FILE-MAGIC-FREEDESKTOP]
 * <mime-type type="application/x-analysis-studio-axd">
 *   <comment>Analysis Studio AXD data</comment>
 *   <glob pattern="*.axd"/>
 *   <glob pattern="*.AXD"/>
 * </mime-type>
 **/

/**
 * [FILE-MAGIC-FREEDESKTOP]
 * <mime-type type="application/x-analysis-studio-axz">
 *   <comment>Analysis Studio AXZ compressed data</comment>
 *   <glob pattern="*.axz"/>
 *   <glob pattern="*.AXZ"/>
 * </mime-type>
 **/

#include <glib/gstdio.h>
#include <app/gwyapp.h>
#include <app/gwymoduleutils-file.h>
#include <libgwyddion/gwymacros.h>
#include <libgwyddion/gwymath.h>
#include <libgwymodule/gwymodule-file.h>
#include <libprocess/stats.h>
#include <libprocess/spectra.h>
#include <stdio.h>
#include <string.h>
#include "err.h"
#include "get.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#define EXTENSION ".axd"
#define EXTENSION2 ".axz"
#define MIN_SIZE 2173
#define MIN_SIZE2 550
#define MAGIC "a\0n\0a\0s\0y\0s\0i\0n\0s\0t\0r\0u\0m\0e\0n\0t\0s\0.\0c\0o\0m\0"
#define MAGIC2 "\x1F\x8B\x08\x00\x00\x00\x00\x00\x04\x00"
#define MAGIC_SIZE (sizeof(MAGIC) - 1)
#define MAGIC2_SIZE (sizeof(MAGIC2) - 1)

/* Only ever pass ASCII strings.  So the typecasting, mean to catch signed vs.
 * unsigned char problems, is not useful, just annoying. */
#define strequal(a, b) xmlStrEqual((a), (const xmlChar*)(b))
#define getprop(elem, name) xmlGetProp((elem), (const xmlChar*)(name))

static gboolean      module_register(void);
static gint          anasys_detect  (const GwyFileDetectInfo *fileinfo,
                                     gboolean only_name);
static GwyContainer* anasys_load    (const gchar *filename,
                                     GwyRunType mode,
                                     GError **error);
static guint32       readHeightMaps (GwyContainer *container,
                                     xmlDoc *doc,
                                     const xmlNode *curNode,
                                     const gchar *filename,
                                     GError **error);
static gboolean      readSpectra    (GwyContainer *container,
                                     xmlDoc *doc,
                                     const xmlNode *curNode);

const gdouble PI_over_180          = G_PI / 180.0;

static GwyModuleInfo module_info = {
    GWY_MODULE_ABI_VERSION,
    &module_register,
    N_("Imports Analysis Studio XML (.axz & .axd) files."),
    "Jeffrey J. Schwartz <schwartz@physics.ucla.edu>",
    "0.6",
    "Jeffrey J. Schwartz",
    "September 2018",
};

GWY_MODULE_QUERY(module_info)

static gboolean
module_register(void)
{
    gwy_file_func_register("anasys_xml",
                           N_("Analysis Studio XML (.axz, .axd)"),
                           (GwyFileDetectFunc)&anasys_detect,
                           (GwyFileLoadFunc)&anasys_load,
                           NULL,
                           NULL);
    return TRUE;
}

static gint
anasys_detect(const GwyFileDetectInfo *fileinfo,
              gboolean only_name)
{
    if (only_name)
        return (g_str_has_suffix(fileinfo->name_lowercase, EXTENSION) ||
                g_str_has_suffix(fileinfo->name_lowercase, EXTENSION2)) ? 20 : 0;
    /* AXD, plain text XML data files */
    if (fileinfo->buffer_len > MIN_SIZE &&
        g_str_has_suffix(fileinfo->name_lowercase, EXTENSION)) {
        if (gwy_memmem(fileinfo->head+350, 100, MAGIC, MAGIC_SIZE) != NULL)
            return 100;
    }
    /* AXZ, gzip-compressed XML data files */
    if (fileinfo->buffer_len > MIN_SIZE2 &&
        g_str_has_suffix(fileinfo->name_lowercase, EXTENSION2)) {
        if (gwy_memmem(fileinfo->head, 10, MAGIC2, MAGIC2_SIZE) != NULL) {
            return 50;
        }
    }
    return 0;
}

static GwyContainer*
anasys_load(const gchar *filename,
            G_GNUC_UNUSED GwyRunType mode, GError **error)
{
    guint32 valid_images = 0;
    GwyContainer *container = gwy_container_new();
    xmlDoc *doc = xmlReadFile(filename, NULL, XML_PARSE_NOERROR);
    xmlNode *curNode, *rootElement = xmlDocGetRootElement(doc);
    xmlChar *ptDocType = NULL;
    xmlChar *ptVersion = NULL;

    if (rootElement != NULL) {
        if (rootElement->type == XML_ELEMENT_NODE &&
            strequal(rootElement->name, "Document")) {
            ptDocType = getprop(rootElement, "DocType");
            ptVersion = getprop(rootElement, "Version");
            if (strequal(ptDocType, "IR") - strequal(ptVersion, "1.0")) {
                err_FILE_TYPE(error, "Analysis Studio");
                return NULL;
            }
        }
    }
    xmlFree(ptDocType);
    xmlFree(ptVersion);
    for (curNode = rootElement->children; curNode; curNode = curNode->next) {
        if (curNode->type != XML_ELEMENT_NODE)
            continue;
        if (strequal(curNode->name, "HeightMaps"))
            valid_images = readHeightMaps(container, doc, curNode,
                                          filename, error);
        else if (strequal(curNode->name, "RenderedSpectra")) {
            if (!readSpectra(container, doc, curNode))
                valid_images = 0;
        }
    }
    xmlFreeDoc(doc);
    xmlCleanupParser();
    if (valid_images == 0) {
        g_object_unref(container);
        err_NO_DATA(error);
        return NULL;
    }
    return container;
}

static guint32
readHeightMaps(GwyContainer *container, xmlDoc *doc, const xmlNode *curNode,
               const gchar *filename, GError **error)
{
    gchar id[40];
    guint32 imageNum = 0;
    guint32 valid_images = 0;

    gsize decoded_size;
    gdouble *data;
    gdouble width;
    gdouble height;
    gdouble pos_x;
    gdouble pos_y;
    gdouble range_x;
    gdouble range_y;
    gdouble scan_angle;
    gdouble zUnitMultiplier;
    guint32 resolution_x;
    guint32 resolution_y;
    guint32 new_resolution_x;
    guint32 new_resolution_y;
    guint32 num_px;
    guint32 oblique_angle;
    gchar *zUnit;
    gchar *tempStr;
    gchar **endptr;
    guchar *decodedData;
    guchar *base64DataString;
    GwyDataField *dfield;
    GwyDataField *dfield_resample;
    GwyDataField *dfield_rotate;
    GwyDataField *dfield_temp;
    GwyContainer *meta;
    xmlChar *key, *xmlPropValue1, *xmlPropValue2;
    xmlNode *childNode, *posNode, *sizeNode, *resNode, *subNode, *tempNode,
            *tagNode;

    for (childNode = curNode->children;
         childNode;
         childNode = childNode->next) {
        if (childNode->type != XML_ELEMENT_NODE)
            continue;
        ++imageNum;

        decoded_size = 0;
        width = 0.0;
        height = 0.0;
        pos_x = 0.0;
        pos_y = 0.0;
        range_x = 0.0;
        range_y = 0.0;
        scan_angle = 0.0;
        zUnitMultiplier = 1.0;
        resolution_x = 0;
        resolution_y = 0;
        new_resolution_x = 0;
        new_resolution_y = 0;
        num_px = 0;
        oblique_angle = 0;
        zUnit = NULL;
        tempStr = NULL;
        endptr = NULL;
        decodedData = NULL;
        base64DataString = NULL;
        xmlPropValue1 = NULL;
        xmlPropValue2 = NULL;

        xmlPropValue1 = getprop(childNode, "DataChannel");
        meta = gwy_container_new();
        gwy_container_set_const_string_by_name(meta, "DataChannel",
                                               (const guchar*)xmlPropValue1);
        xmlFree(xmlPropValue1);

        for (tempNode = childNode->children;
             tempNode;
             tempNode = tempNode->next) {
            if (tempNode->type != XML_ELEMENT_NODE)
                continue;
            if (strequal(tempNode->name, "Position")) {
                for (posNode = tempNode->children;
                     posNode;
                     posNode = posNode->next) {
                    if (posNode->type != XML_ELEMENT_NODE)
                        continue;
                    key = xmlNodeListGetString(doc,
                                               posNode->xmlChildrenNode, 1);
                    if (strequal(posNode->name, "X"))
                        pos_x = g_ascii_strtod((const gchar*)key, endptr);
                    else if (strequal(posNode->name, "Y"))
                        pos_y = g_ascii_strtod((const gchar*)key, endptr);
                    tempStr = g_strdup_printf("Position_%s", posNode->name);
                    gwy_container_set_const_string_by_name(meta, tempStr,
                                                           (guchar *)key);
                    xmlFree(key);
                    g_free(tempStr);
                }
            }
            else if (strequal(tempNode->name, "Size")) {
                for (sizeNode = tempNode->children;
                     sizeNode;
                     sizeNode = sizeNode->next) {
                    if (sizeNode->type != XML_ELEMENT_NODE)
                        continue;
                    key = xmlNodeListGetString(doc,
                                               sizeNode->xmlChildrenNode, 1);
                    if (strequal(sizeNode->name, "X"))
                        range_x = g_ascii_strtod((const gchar*)key, endptr);
                    else if (strequal(sizeNode->name, "Y"))
                        range_y = g_ascii_strtod((const gchar*)key, endptr);
                    tempStr = g_strdup_printf("Size_%s", sizeNode->name);
                    gwy_container_set_const_string_by_name(meta, tempStr,
                                                           (guchar *)key);
                    xmlFree(key);
                    g_free(tempStr);
                }
            }
            else if (strequal(tempNode->name, "Resolution")) {
                for (resNode = tempNode->children;
                     resNode;
                     resNode = resNode->next) {
                    if (resNode->type != XML_ELEMENT_NODE)
                        continue;
                    key = xmlNodeListGetString(doc,
                                               resNode->xmlChildrenNode, 1);
                    if (strequal(resNode->name, "X"))
                        resolution_x = (gint32)atoi((char *)key);
                    else if (strequal(resNode->name, "Y"))
                        resolution_y = (gint32)atoi((char *)key);
                    tempStr = g_strdup_printf("Resolution_%s", resNode->name);
                    gwy_container_set_const_string_by_name(meta, tempStr,
                                                           (guchar *)key);
                    xmlFree(key);
                    g_free(tempStr);
                }
            }
            else if (strequal(tempNode->name, "Units")) {
                key = xmlNodeListGetString(doc,
                                           tempNode->xmlChildrenNode, 1);
                zUnit = g_strdup((gchar*)key);
                gwy_container_set_const_string_by_name(meta,
                                                   "Units", (guchar *)zUnit);
                xmlFree(key);
            }
            else if (strequal(tempNode->name, "UnitPrefix")) {
                key = xmlNodeListGetString(doc,
                                           tempNode->xmlChildrenNode, 1);
                if (strequal(key, "f"))
                    zUnitMultiplier = 1.0e-15;
                else if (strequal(key, "p"))
                    zUnitMultiplier = 1.0e-12;
                else if (strequal(key, "n"))
                    zUnitMultiplier = 1.0e-9;
                else if (strequal(key, "u"))
                    zUnitMultiplier = 1.0e-6;
                else if (strequal(key, "m"))
                    zUnitMultiplier = 1.0e-3;
                xmlFree(key);
            }
            else if (strequal(tempNode->name, "Tags")) {
                for (tagNode = tempNode->children;
                     tagNode;
                     tagNode = tagNode->next) {
                    if (tagNode->type != XML_ELEMENT_NODE)
                        continue;
                    xmlPropValue1 = getprop(tagNode, "Name");
                    if (strequal(xmlPropValue1, "ScanAngle")) {
                        gchar *p;
                        xmlPropValue2 = getprop(tagNode, "Value");
                        p = strchr((const gchar*)xmlPropValue2, ' ');
                        if (p) {
                            p = '\0';
                            scan_angle = g_ascii_strtod(
                                                  (const gchar*)xmlPropValue2,
                                                  endptr);
                            while (scan_angle > 180.0)
                                scan_angle -= 360.0;
                            while (scan_angle <= -180.0)
                                scan_angle += 360.0;
                        }
                        else
                            scan_angle = 0.0;
                        xmlFree(xmlPropValue2);
                    }
                    xmlFree(xmlPropValue1);
                    xmlPropValue1 = getprop(tagNode, "Name");
                    xmlPropValue2 = getprop(tagNode, "Value");
                    gwy_container_set_const_string_by_name(meta,
                                                  (const gchar*)xmlPropValue1,
                                                  (guchar*)xmlPropValue2);
                    xmlFree(xmlPropValue1);
                    xmlFree(xmlPropValue2);
                }
            }
            else if (strequal(tempNode->name, "SampleBase64")) {
                key = xmlNodeListGetString(doc, tempNode->xmlChildrenNode, 1);
                base64DataString = (guchar*)g_strdup((gchar*)key);
                xmlFree(key);
            }
            else {
                if (xmlChildElementCount(tempNode) == 0) {
                    key = xmlNodeListGetString(doc,
                                        tempNode->xmlChildrenNode, 1);
                    gwy_container_set_const_string_by_name(meta,
                                  (const gchar*)tempNode->name, (guchar*)key);
                    xmlFree(key);
                }
                else {
                    for (subNode = tempNode->children;
                         subNode;
                         subNode = subNode->next) {
                        if (subNode->type != XML_ELEMENT_NODE)
                            continue;
                        key = xmlNodeListGetString(doc,
                                        subNode->xmlChildrenNode, 1);
                        tempStr = g_strdup_printf("%s_%s",
                                                  tempNode->name,
                                                  subNode->name);
                        gwy_container_set_const_string_by_name(meta, tempStr,
                                                               (guchar*)key);
                        g_free(tempStr);
                        xmlFree(key);
                    }
                }
            }
        }

        if (!base64DataString) {
            g_object_unref(meta);
            g_free(zUnit);
            continue;
        }

        num_px = resolution_x * resolution_y;
        if (num_px < 1) {
            g_free(base64DataString);
            g_object_unref(meta);
            g_free(zUnit);
            continue;
        }

        dfield = gwy_data_field_new(resolution_x, resolution_y,
                                    range_x*1.0e-6, range_y*1.0e-6, FALSE);
        gwy_si_unit_set_from_string(gwy_data_field_get_si_unit_xy(dfield),
                                    "m");
        gwy_si_unit_set_from_string(gwy_data_field_get_si_unit_z(dfield),
                                    zUnit);

        data = gwy_data_field_get_data(dfield);

        decodedData = g_base64_decode((const gchar*)base64DataString,
                                      &decoded_size);
        if (err_SIZE_MISMATCH(error, sizeof(gfloat)*num_px, decoded_size,
                              TRUE)) {
            g_object_unref(dfield);
            g_free(zUnit);
            g_free(decodedData);
            g_free(base64DataString);
            continue;
        }
        gwy_convert_raw_data(decodedData, num_px, 1,
                             GWY_RAW_DATA_FLOAT, GWY_BYTE_ORDER_LITTLE_ENDIAN,
                             data, zUnitMultiplier, 0.0);

        if (scan_angle == 0.0) {
            gwy_data_field_invert(dfield, TRUE, FALSE, FALSE);
            width = range_x;
            height = range_y;
        }
        else if (scan_angle == 180.0) {
            gwy_data_field_invert(dfield, FALSE, TRUE, FALSE);
            width = range_x;
            height = range_y;
        }
        else if (scan_angle == 90.0) {
            dfield_temp = dfield;
            dfield = gwy_data_field_new_rotated_90(dfield, FALSE);
            g_object_unref(dfield_temp);
            gwy_data_field_invert(dfield, TRUE, FALSE, FALSE);
            width = range_y;
            height = range_x;
        }
        else if (scan_angle == -90.0) {
            dfield_temp = dfield;
            dfield = gwy_data_field_new_rotated_90(dfield, TRUE);
            g_object_unref(dfield_temp);
            gwy_data_field_invert(dfield, TRUE, FALSE, FALSE);
            width = range_y;
            height = range_x;
        }
        else {
            const gdouble rot_angle = PI_over_180 * scan_angle;
            dfield_resample = dfield;
            if ( (resolution_x > 128) || (resolution_y > 128) ) {
                new_resolution_x = GWY_ROUND(0.125*resolution_x);
                new_resolution_y = GWY_ROUND(0.125*resolution_y);
                new_resolution_x = CLAMP(new_resolution_x, 16, 128);
                new_resolution_y = CLAMP(new_resolution_y, 16, 128);
                dfield_resample = gwy_data_field_new_resampled(dfield,
                                                new_resolution_x,
                                                new_resolution_y,
                                                GWY_INTERPOLATION_BSPLINE);
            }
            dfield_rotate = gwy_data_field_new_rotated(dfield_resample,
                                            NULL, rot_angle,
                                            GWY_INTERPOLATION_BSPLINE,
                                            GWY_ROTATE_RESIZE_EXPAND);
            gwy_data_field_invert(dfield_rotate, TRUE, FALSE, FALSE);
            width = gwy_data_field_get_xreal(dfield_rotate);
            height = gwy_data_field_get_yreal(dfield_rotate);
            oblique_angle = 1;
        }

        if (oblique_angle) {
            gwy_data_field_set_xoffset(dfield, 1.0);
            gwy_data_field_set_yoffset(dfield, 1.0);
            gwy_data_field_set_xoffset(dfield_rotate,
                                        pos_x*1.0e-6 - 0.5*width);
            gwy_data_field_set_yoffset(dfield_rotate,
                                        pos_y*1.0e-6 - 0.5*height);
        }
        else {
            gwy_data_field_set_xoffset(dfield,
                                        (pos_x - 0.5*width)*1.0e-6);
            gwy_data_field_set_yoffset(dfield,
                                        (pos_y - 0.5*height)*1.0e-6);
        }

        g_snprintf(id, sizeof(id), "/%i/data", imageNum);
        gwy_container_set_object_by_name(container, id, dfield);
        g_snprintf(id, sizeof(id), "/%i/meta", imageNum);
        gwy_container_set_object_by_name(container, id, meta);

        if (oblique_angle) {
            g_snprintf(id, sizeof(id), "/%i/data", 1000000 + imageNum);
            gwy_container_set_object_by_name(container, id, dfield_rotate);
            g_snprintf(id, sizeof(id), "/%i/meta", 1000000 + imageNum);
            gwy_container_set_object_by_name(container, id, meta);
            g_snprintf(id, sizeof(id), "/%i/data/title", 1000000 + imageNum);
            xmlPropValue1 = getprop(childNode, "Label");
            tempStr = g_strdup_printf("%s (Rotated)", xmlPropValue1);
            gwy_container_set_const_string_by_name(container, id,
                                                   (guchar*)tempStr);
            xmlFree(xmlPropValue1);
            g_free(tempStr);
            g_snprintf(id, sizeof(id), "/%i/data/title", imageNum);
            xmlPropValue1 = getprop(childNode, "Label");
            tempStr = g_strdup_printf("%s (Offset)", xmlPropValue1);
            gwy_container_set_const_string_by_name(container, id,
                                                   (guchar*)tempStr);
            xmlFree(xmlPropValue1);
            g_free(tempStr);
            g_object_unref(dfield_rotate);
            g_object_unref(dfield_resample);
        }
        else {
            xmlPropValue1 = getprop(childNode, "Label");
            g_snprintf(id, sizeof(id), "/%i/data/title", imageNum);
            gwy_container_set_const_string_by_name(container, id,
                                                  (const guchar*)xmlPropValue1);
            xmlFree(xmlPropValue1);
        }
        gwy_app_channel_check_nonsquare(container, imageNum);
        gwy_file_channel_import_log_add(container, imageNum, NULL, filename);
        ++valid_images;

        g_object_unref(meta);
        g_object_unref(dfield);
        g_free(zUnit);
        g_free(decodedData);
        g_free(base64DataString);
    }

    return valid_images;
}

static gboolean
readSpectra(GwyContainer *container, xmlDoc *doc,
            const xmlNode *curNode)
{
    gchar id[40];
    guint32 specID = 0;

    xmlChar *key;
    xmlChar *xmlPropValue1;
    xmlNode *dcNode;
    xmlNode *locNode;
    xmlNode *subNode;
    xmlNode *childNode;
    gsize decoded_size;
    gdouble location_x;
    gdouble location_y;
    gdouble startWavenum;
    gdouble endWavenum;
    guint32 numDataPoints;
    guchar *base64SpecString;
    guchar *decodedData;
    gchar *tempStr;
    gchar *label = NULL;
    gchar *polarization = NULL;
    gchar *channelName = NULL;
    gchar **endptr;
    gdouble *ydata;
    GwyDataLine *dataline;
    GwyDataLine *copy_dataline;
    GwySpectra *spectra;

    GwySpectra *spectra_all = gwy_spectra_new();
    gwy_si_unit_set_from_string(gwy_spectra_get_si_unit_xy(spectra_all), "m");
    gwy_spectra_set_spectrum_x_label(spectra_all,
                                     "Wavenumber (cm<sup>-1</sup>)");
    gwy_spectra_set_title(spectra_all, "All Spectra (Polarization): DataChannel");
    for (childNode = curNode->children;
         childNode;
         childNode = childNode->next) {
        if (childNode->type != XML_ELEMENT_NODE)
            continue;
        if (strequal(childNode->name, "IRRenderedSpectra") == 0)
            continue;

        endptr = 0;
        xmlPropValue1 = NULL;
        decoded_size = 0;
        location_x = 0.0;
        location_y = 0.0;
        startWavenum = 0.0;
        endWavenum = 0.0;
        numDataPoints = 0;

        for (subNode = childNode->children; subNode; subNode = subNode->next) {
            if (subNode->type != XML_ELEMENT_NODE)
                continue;
            if (strequal(subNode->name, "Label")) {
                key = xmlNodeListGetString(doc, subNode->xmlChildrenNode, 1);
                label = g_strdup((gchar*)key);
                xmlFree(key);
            }
            else if (strequal(subNode->name, "DataPoints")) {
                key = xmlNodeListGetString(doc,
                                           subNode->xmlChildrenNode, 1);
                numDataPoints = (guint32)atoi((char*)key);
                xmlFree(key);
            }
            else if (strequal(subNode->name, "StartWavenumber")) {
                key = xmlNodeListGetString(doc, subNode->xmlChildrenNode, 1);
                startWavenum = g_ascii_strtod((const gchar*)key, endptr);
                xmlFree(key);
            }
            else if (strequal(subNode->name, "EndWavenumber")) {
                key = xmlNodeListGetString(doc, subNode->xmlChildrenNode, 1);
                endWavenum = g_ascii_strtod((const gchar*)key, endptr);
                xmlFree(key);
            }
            else if (strequal(subNode->name, "Polarization")) {
                key = xmlNodeListGetString(doc, subNode->xmlChildrenNode, 1);
                polarization = g_strdup((gchar*)key);
                xmlFree(key);
            }
            else if (strequal(subNode->name, "Location")) {
                for (locNode = subNode->children;
                     locNode;
                     locNode = locNode->next) {
                    if (locNode->type != XML_ELEMENT_NODE)
                        continue;
                    key = xmlNodeListGetString(doc,
                                               locNode->xmlChildrenNode, 1);
                    if (strequal(locNode->name, "X"))
                        location_x = g_ascii_strtod((const gchar*)key, endptr);
                    else if (strequal(locNode->name, "Y"))
                        location_y = g_ascii_strtod((const gchar*)key, endptr);
                    xmlFree(key);
                }
            }
            else if (strequal(subNode->name, "DataChannels")) {
                ++specID;
                base64SpecString = NULL;
                spectra = gwy_spectra_new();
                gwy_si_unit_set_from_string(gwy_spectra_get_si_unit_xy(spectra), "m");
                gwy_spectra_set_spectrum_x_label(spectra,
                                                 "Wavenumber (cm<sup>-1</sup>)");

                xmlPropValue1 = getprop(subNode, "DataChannel");
                gwy_spectra_set_spectrum_y_label(spectra,
                                                 (gchar*)xmlPropValue1);
                channelName = g_strdup((gchar*)xmlPropValue1);
                xmlFree(xmlPropValue1);
                for (dcNode = subNode->children;
                     dcNode;
                     dcNode = dcNode->next) {
                    if (dcNode->type != XML_ELEMENT_NODE)
                        continue;
                    if (strequal(dcNode->name, "SampleBase64")) {
                        key = xmlNodeListGetString(doc,
                                                   dcNode->xmlChildrenNode, 1);
                        base64SpecString = (guchar*)g_strdup((gchar*)key);
                        xmlFree(key);
                        break;
                    }
                }

                tempStr = g_strdup_printf("%s (%s): %s", label, polarization, channelName);
                gwy_spectra_set_title(spectra, tempStr);
                g_free(channelName);
                g_free(tempStr);

                if (!base64SpecString) {
                    g_object_unref(spectra);
                    continue;
                }
                if (numDataPoints < 1) {
                    g_object_unref(spectra);
                    g_free(base64SpecString);
                    continue;
                }
                decodedData = g_base64_decode((const gchar*)base64SpecString,
                                              &decoded_size);
                numDataPoints = decoded_size / sizeof(gfloat);
                if (numDataPoints < 1) {
                    g_object_unref(spectra);
                    g_free(decodedData);
                    g_free(base64SpecString);
                    continue;
                }
                dataline = gwy_data_line_new(numDataPoints,
                    (endWavenum-startWavenum)*(1.0+(1.0/((gdouble)numDataPoints-1.0))),
                    TRUE);
                gwy_data_line_set_offset(dataline, startWavenum);
                ydata = gwy_data_line_get_data(dataline);
                gwy_convert_raw_data(decodedData, numDataPoints, 1,
                                     GWY_RAW_DATA_FLOAT, GWY_BYTE_ORDER_LITTLE_ENDIAN,
                                     ydata, 1.0, 0.0);
                g_free(decodedData);

                copy_dataline = gwy_data_line_duplicate(dataline);
                gwy_spectra_add_spectrum(spectra, dataline,
                                         location_x*1.0e-6, location_y*1.0e-6);
                gwy_spectra_add_spectrum(spectra_all, copy_dataline,
                                         location_x*1.0e-6, location_y*1.0e-6);

                g_snprintf(id, sizeof(id), "/sps/%i", specID);
                gwy_container_set_object_by_name(container, id, spectra);

                g_object_unref(spectra);
                g_object_unref(dataline);
                g_object_unref(copy_dataline);
                g_free(base64SpecString);
            }
        }
        g_free(label);
        g_free(polarization);
    }
    if (specID > 0)
        gwy_container_set_object_by_name(container, "/sps/0", spectra_all);

    g_object_unref(spectra_all);

    return TRUE;
}
