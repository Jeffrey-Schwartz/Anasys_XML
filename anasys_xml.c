/*
 *  @(#) $Id: anasys_xml.c 2018-10-15 $
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
#include "stdint.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <cdecode.h>

#define EXTENSION ".axd"
#define MAGIC_SIZE 2173

static gboolean      module_register (void);
static gint          anasys_detect   (const GwyFileDetectInfo *fileinfo,
                                      gboolean only_name);
static GwyContainer* anasys_load     (const gchar *filename,
                                      GwyRunType mode,
                                      GError **error);
static void          readHeightMaps  (GwyContainer *container, xmlDoc *doc,
                                      const xmlNode *curNode);
static void          readSpectra     (GwyContainer *container, xmlDoc *doc,
                                      const xmlNode *curNode);
static guchar*       decodeBase64    (const guchar* input);

const xmlChar* DATA_CHANNEL        = (const xmlChar *)"DataChannel";
const xmlChar* DATA_CHANNELS       = (const xmlChar *)"DataChannels";
const xmlChar* DATA_POINTS         = (const xmlChar *)"DataPoints";
const xmlChar* END_WAVENUMBER      = (const xmlChar *)"EndWavenumber";
const xmlChar* FEMTO               = (const xmlChar *)"f";
const xmlChar* HEIGHT_MAPS         = (const xmlChar *)"HeightMaps";
const xmlChar* IR_RENDERED_SPECTRA = (const xmlChar *)"IRRenderedSpectra";
const xmlChar* LABEL               = (const xmlChar *)"Label";
const xmlChar* LOCATION            = (const xmlChar *)"Location";
const xmlChar* MICRO               = (const xmlChar *)"u";
const xmlChar* MILLI               = (const xmlChar *)"m";
const xmlChar* NAME                = (const xmlChar *)"Name";
const xmlChar* NANO                = (const xmlChar *)"n";
const xmlChar* PICO                = (const xmlChar *)"p";
const xmlChar* POSITION            = (const xmlChar *)"Position";
const xmlChar* RENDERED_SPECTRA    = (const xmlChar *)"RenderedSpectra";
const xmlChar* RESOLUTION          = (const xmlChar *)"Resolution";
const xmlChar* SAMPLE_BASE_64      = (const xmlChar *)"SampleBase64";
const xmlChar* SCAN_ANGLE          = (const xmlChar *)"ScanAngle";
const xmlChar* SIZE                = (const xmlChar *)"Size";
const xmlChar* START_WAVENUMBER    = (const xmlChar *)"StartWavenumber";
const xmlChar* TAGS                = (const xmlChar *)"Tags";
const xmlChar* UNITS               = (const xmlChar *)"Units";
const xmlChar* UNIT_PREFIX         = (const xmlChar *)"UnitPrefix";
const xmlChar* VALUE               = (const xmlChar *)"Value";
const xmlChar* X                   = (const xmlChar *)"X";
const xmlChar* Y                   = (const xmlChar *)"Y";

const gdouble PI_over_180          = G_PI / 180.0;

static GwyModuleInfo module_info = {
    GWY_MODULE_ABI_VERSION,
    &module_register,
    N_("Imports Analysis Studio XML (.axd) files."),
    "Jeffrey J. Schwartz <schwartz@physics.ucla.edu>",
    "0.5",
    "Jeffrey J. Schwartz",
    "September 2018",
};
GWY_MODULE_QUERY(module_info)

static gboolean
module_register(void)
{
    gwy_file_func_register("Analysis_Studio",
                           N_("Analysis Studio XML (.axd)"),
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
    xmlParserCtxtPtr ctxt;
    xmlDocPtr doc;
    gint returnCode = 0;

    if (only_name)
        return g_str_has_suffix(fileinfo->name_lowercase, EXTENSION) ? 20 : 0;
    if (fileinfo->buffer_len > MAGIC_SIZE &&
        g_str_has_suffix(fileinfo->name_lowercase, EXTENSION))
    {    
        ctxt = xmlNewParserCtxt();
        doc = xmlCtxtReadFile(ctxt, fileinfo->name, NULL, XML_PARSE_RECOVER);
        if (doc == NULL ||
            ctxt->wellFormed == 0 ||
            memcmp(ctxt->encoding, "utf-16", sizeof("utf-16")) ||
            memcmp(ctxt->version, "1.0", sizeof("1.0")) )
        {
            returnCode = 0;
        }
        else
        {
            returnCode = 50;        
        }
        xmlFreeDoc(doc);
        xmlFreeParserCtxt(ctxt);
    }
    return returnCode;
}

static GwyContainer*
anasys_load(const gchar *filename,
            G_GNUC_UNUSED GwyRunType mode, GError **error)
{
    GwyContainer *container = gwy_container_new();
    xmlDoc *doc = xmlReadFile(filename, NULL, XML_PARSE_NOERROR);
    xmlNode *rootElement = xmlDocGetRootElement(doc);
    if (rootElement != NULL)
    {
        if (rootElement->type == XML_ELEMENT_NODE &&
            xmlStrEqual(rootElement->name, (const xmlChar *)"Document"))
        {
            if (xmlStrEqual(xmlGetProp(rootElement,
                                        (const xmlChar *)"DocType"),
                                        (const xmlChar *)"IR") -
                xmlStrEqual(xmlGetProp(rootElement,
                                        (const xmlChar *)"Version"),
                                        (const xmlChar *)"1.0"))
            {
                err_FILE_TYPE(error, "Analysis Studio");
                return NULL;
            }
        }
    }
    for (xmlNode *curNode = rootElement->children;
         curNode; curNode = curNode->next)
    {
        if (curNode->type != XML_ELEMENT_NODE)
            continue;
        if (xmlStrEqual(curNode->name, HEIGHT_MAPS))
            readHeightMaps(container, doc, curNode);
        else if (xmlStrEqual(curNode->name, RENDERED_SPECTRA))
            readSpectra(container, doc, curNode);
    }
    xmlFreeDoc(doc);
    xmlCleanupParser();
    gwy_file_channel_import_log_add(container, 0, "Analysis_Studio", filename);
    return container;
}

static void
readHeightMaps(GwyContainer *container, xmlDoc *doc, const xmlNode *curNode)
{
    gchar id[40];
    guint32 imageNum = 0;
    for (xmlNode *childNode = curNode->children;
         childNode; childNode = childNode->next)
    {
        if (childNode->type != XML_ELEMENT_NODE)
            continue;
        ++imageNum;

        gdouble *data;
        //gdouble px_x;
        //gdouble px_y;
        gdouble width = 0.0;
        gdouble height = 0.0;
        gdouble pos_x = 0.0;
        gdouble pos_y = 0.0;
        gdouble range_x = 0.0;
        gdouble range_y = 0.0;
        gdouble scan_angle = 0.0;
        gdouble zUnitMultiplier = 1.0;
        guint32 resolution_x = 0;
        guint32 resolution_y = 0;
        guint32 oblique_angle = 0;
        gchar *zUnit = "m";
        const guchar *readMarker;
        guchar *decodedData = 0;
        guchar *base64DataString = 0;
        GwyDataField *dfield;
        GwyDataField *dfield_rotate;
        GwyContainer *meta = gwy_container_new();
        gwy_container_set_string_by_name(meta, "DataChannel",
            (const guchar *)xmlGetProp(childNode, DATA_CHANNEL));

        for (xmlNode *tempNode = childNode->children;
             tempNode; tempNode = tempNode->next)
        {
            if (tempNode->type != XML_ELEMENT_NODE)
                continue;
            if (xmlStrEqual(tempNode->name, POSITION))
            {
                for (xmlNode *posNode = tempNode->children;
                     posNode; posNode = posNode->next)
                {
                    if (posNode->type != XML_ELEMENT_NODE)
                        continue;
                    xmlChar *key = xmlNodeListGetString(doc,
                                        posNode->xmlChildrenNode, 1);
                    if (xmlStrEqual(posNode->name, X))
                        pos_x = (gdouble)atof((char *)key);                       
                    else if (xmlStrEqual(posNode->name, Y))
                        pos_y = (gdouble)atof((char *)key);                           
                    gwy_container_set_string_by_name(meta,
                            (const gchar *)g_strdup_printf("Position_%s",
                            posNode->name), (guchar *)strdup((gchar *)key));
                    xmlFree(key);
                }
            }
            else if (xmlStrEqual(tempNode->name, SIZE))
            {
                for (xmlNode *sizeNode = tempNode->children;
                     sizeNode; sizeNode = sizeNode->next)
                {
                    if (sizeNode->type != XML_ELEMENT_NODE)
                        continue;
                    xmlChar *key = xmlNodeListGetString(doc,
                                        sizeNode->xmlChildrenNode, 1);
                    if (xmlStrEqual(sizeNode->name, X))
                        range_x = (gdouble)atof((char *)key);
                    else if (xmlStrEqual(sizeNode->name, Y))
                        range_y = (gdouble)atof((char *)key);
                    gwy_container_set_string_by_name(meta,
                            (const gchar *)g_strdup_printf("Size_%s",
                            sizeNode->name), (guchar *)strdup((gchar *)key));
                    xmlFree(key);
                }
            }
            else if (xmlStrEqual(tempNode->name, RESOLUTION))
            {
                for (xmlNode *resNode = tempNode->children;
                     resNode; resNode = resNode->next)
                {
                    if (resNode->type != XML_ELEMENT_NODE)
                        continue;
                    xmlChar *key = xmlNodeListGetString(doc,
                                        resNode->xmlChildrenNode, 1);
                    if (xmlStrEqual(resNode->name, X))
                        resolution_x = (gint32)atoi((char *)key);                            
                    else if (xmlStrEqual(resNode->name, Y))
                        resolution_y = (gint32)atoi((char *)key);                            
                    gwy_container_set_string_by_name(meta,
                            (const gchar *)g_strdup_printf("Resolution_%s",
                            resNode->name), (guchar *)strdup((gchar *)key));
                    xmlFree(key);
                }
            }
            else if (xmlStrEqual(tempNode->name, UNITS))
            {
                xmlChar *key = xmlNodeListGetString(doc,
                                    tempNode->xmlChildrenNode, 1);
                zUnit = strdup((char *)key);
                gwy_container_set_string_by_name(meta,
                    "Units", (guchar *)zUnit);
                xmlFree(key);
            }
            else if (xmlStrEqual(tempNode->name, UNIT_PREFIX))
            {
                xmlChar *key = xmlNodeListGetString(doc,
                                    tempNode->xmlChildrenNode, 1);
                if (xmlStrEqual(key, FEMTO))
                    zUnitMultiplier = 1.0E-15;
                else if (xmlStrEqual(key, PICO))
                    zUnitMultiplier = 1.0E-12;
                else if (xmlStrEqual(key, NANO))
                    zUnitMultiplier = 1.0E-9;
                else if (xmlStrEqual(key, MICRO))
                    zUnitMultiplier = 1.0E-6;
                else if (xmlStrEqual(key, MILLI))
                    zUnitMultiplier = 1.0E-3;
                xmlFree(key);
            }
            else if (xmlStrEqual(tempNode->name, TAGS))
            {
                for (xmlNode *tagNode = tempNode->children;
                     tagNode; tagNode = tagNode->next)
                {
                    if (tagNode->type != XML_ELEMENT_NODE)
                        continue;
                    if (xmlStrEqual(xmlGetProp(tagNode, NAME), SCAN_ANGLE))
                    {
                        gchar value[30];
                        g_snprintf(value, sizeof(value), "%s ",
                                   xmlGetProp(tagNode, VALUE));
                        char *p = strchr(value, ' ');
                        if (p)
                        {
                            p = '\0';
                            scan_angle = (gdouble)atof(value);
                            while (scan_angle > 180.0)
                                scan_angle -= 360.0;
                            while (scan_angle <= -180.0)
                                scan_angle += 360.0;
                        }
                    }
                    gwy_container_set_string_by_name(meta,
                        (const gchar *)xmlGetProp(tagNode, NAME),
                        (guchar *)xmlGetProp(tagNode, VALUE));
                }
            }
            else if (xmlStrEqual(tempNode->name, SAMPLE_BASE_64))
            {
                xmlChar *key = xmlNodeListGetString(doc,
                                        tempNode->xmlChildrenNode, 1);
                base64DataString = (guchar *)strdup((gchar *)key);
                xmlFree(key);
            }
            else
            {
                xmlChar *key = NULL;
                if (xmlChildElementCount(tempNode) == 0)
                {
                    key = xmlNodeListGetString(doc,
                                        tempNode->xmlChildrenNode, 1);
                    gwy_container_set_string_by_name(meta,
                        (const gchar *)tempNode->name,
                        (guchar *)strdup((gchar *)key));
                }
                else
                {
                    for (xmlNode *subNode = tempNode->children;
                         subNode; subNode = subNode->next)
                    {
                        if (subNode->type != XML_ELEMENT_NODE)
                            continue;
                        key = xmlNodeListGetString(doc,
                                        subNode->xmlChildrenNode, 1);
	                    gwy_container_set_string_by_name(meta,
                            (const gchar *)g_strdup_printf("%s_%s",
                                                           tempNode->name,
                                                           subNode->name),
                            (guchar *)strdup((gchar *)key));
                    }
                }
                xmlFree(key);
            }
        }

        dfield = gwy_data_field_new(resolution_x, resolution_y,
                                    range_x*1.0E-6, range_y*1.0E-6, FALSE);
        gwy_si_unit_set_from_string(gwy_data_field_get_si_unit_xy(dfield), "m");
        gwy_si_unit_set_from_string(gwy_data_field_get_si_unit_z(dfield), zUnit);

        data = gwy_data_field_get_data(dfield);

        decodedData = decodeBase64(base64DataString);
        readMarker = decodedData;
        for (guint32 k = 0; k < resolution_x * resolution_y; ++k)
            data[k] = gwy_get_gfloat_le(&readMarker);
        gwy_data_field_multiply(dfield, zUnitMultiplier);

        /*px_x = range_x / resolution_x;
        px_y = range_y / resolution_y;
        if (px_x > px_y)
        {
            const guint32 new_res_x = MAX(GWY_ROUND(range_x / px_y), 1);
            gwy_data_field_resample(dfield, new_res_x, resolution_y,
                                    GWY_INTERPOLATION_BSPLINE);
        }
        else if (px_x < px_y)
        {
            const guint32 new_res_y = MAX(GWY_ROUND(range_y / px_x), 1);
            gwy_data_field_resample(dfield, resolution_x, new_res_y,
                                    GWY_INTERPOLATION_BSPLINE);
        }*/

        if (scan_angle == 0.0)
        {
            gwy_data_field_invert(dfield, TRUE, FALSE, FALSE);
            width = range_x;
            height = range_y;
        }
        else if (scan_angle == 180.0)
        {
            gwy_data_field_invert(dfield, FALSE, TRUE, FALSE);
            width = range_x;
            height = range_y;
        }
        else if (scan_angle == 90.0)
        {
            dfield = gwy_data_field_new_rotated_90(dfield, FALSE);
            gwy_data_field_invert(dfield, TRUE, FALSE, FALSE);
            width = range_y;
            height = range_x;
        }
        else if (scan_angle == -90.0)
        {
            dfield = gwy_data_field_new_rotated_90(dfield, TRUE);
            gwy_data_field_invert(dfield, TRUE, FALSE, FALSE);
            width = range_y;
            height = range_x;
        }
        else
        {
            const gdouble rot_angle = PI_over_180 * scan_angle;
            dfield_rotate = gwy_data_field_new_rotated(dfield, NULL, rot_angle,
                        GWY_INTERPOLATION_BSPLINE, GWY_ROTATE_RESIZE_EXPAND);
            gwy_data_field_invert(dfield_rotate, TRUE, FALSE, FALSE);
            width = gwy_data_field_get_xreal(dfield_rotate);
            height = gwy_data_field_get_yreal(dfield_rotate);
            oblique_angle = 1;
        }

        if (oblique_angle)
        {
            gwy_data_field_set_xoffset(dfield, 1.0);
            gwy_data_field_set_yoffset(dfield, 1.0);
            gwy_data_field_set_xoffset(dfield_rotate,
                                        pos_x*1.0E-6 - 0.5*width);
            gwy_data_field_set_yoffset(dfield_rotate,
                                        pos_y*1.0E-6 - 0.5*height);
        }
        else
        {
            gwy_data_field_set_xoffset(dfield,
                                        (pos_x - 0.5*width)*1.0E-6);
            gwy_data_field_set_yoffset(dfield,
                                        (pos_y - 0.5*height)*1.0E-6);
        }

        g_snprintf(id, sizeof(id), "/%i/data", imageNum);
        gwy_container_set_object_by_name(container, id, dfield);
        g_snprintf(id, sizeof(id), "/%i/meta", imageNum);
        gwy_container_set_object_by_name(container, id, meta);

        if (oblique_angle)
        {
            g_snprintf(id, sizeof(id), "/%i/data", 1000000 + imageNum);
            gwy_container_set_object_by_name(container, id, dfield_rotate);
            g_snprintf(id, sizeof(id), "/%i/meta", 1000000 + imageNum);
            gwy_container_set_object_by_name(container, id, meta);
            g_snprintf(id, sizeof(id), "/%i/data/title", 1000000 + imageNum);
            gwy_container_set_string_by_name(container, id,
                        (const guchar *)g_strdup_printf("%s (Rotated)",
                        xmlGetProp(childNode, LABEL)));
            g_snprintf(id, sizeof(id), "/%i/data/title", imageNum);
            gwy_container_set_string_by_name(container, id,
                        (const guchar *)g_strdup_printf("%s (Offset)",
                        xmlGetProp(childNode, LABEL)));
            g_object_unref(dfield_rotate);
        }
        else
        {
            g_snprintf(id, sizeof(id), "/%i/data/title", imageNum);
            gwy_container_set_string_by_name(container, id,
                            (const guchar *)xmlGetProp(childNode, LABEL));
        }

        g_object_unref(meta);
        g_object_unref(dfield);
        g_free(decodedData);
        g_free(base64DataString);
    }
}

static void
readSpectra(GwyContainer *container, xmlDoc *doc, const xmlNode *curNode)
{
    gchar id[40];
    guint32 specNum = 0;
    GwySpectra *spectra_all = gwy_spectra_new();
    gwy_si_unit_set_from_string(gwy_spectra_get_si_unit_xy(spectra_all), "m");
    gwy_spectra_set_spectrum_x_label(spectra_all, "Wavenumber (cm<sup>-1</sup>)");
    gwy_spectra_set_title(spectra_all, "All Spectra");
    for (xmlNode *childNode = curNode->children; childNode; childNode = childNode->next)
    {
        if (childNode->type != XML_ELEMENT_NODE)
            continue;
        if (xmlStrEqual(childNode->name, IR_RENDERED_SPECTRA) == 0)
            continue;
        ++specNum;

        gdouble location_x = 0.0;
        gdouble location_y = 0.0;
        gdouble startWavenum = 0.0;
        gdouble endWavenum = 0.0;
        guint32 numDataPoints = 0;
        guchar *base64SpecString = 0;
        guchar *decodedData = 0;
        const guchar *readMarker;
        gdouble *ydata;
        GwyDataLine *dataline;
        GwySpectra *spectra = gwy_spectra_new();
        gwy_si_unit_set_from_string(gwy_spectra_get_si_unit_xy(spectra), "m");
        gwy_spectra_set_spectrum_x_label(spectra, "Wavenumber (cm<sup>-1</sup>)");
        
        for (xmlNode *subNode = childNode->children;
             subNode; subNode = subNode->next)
        {
            if (subNode->type != XML_ELEMENT_NODE)
                continue;
            if (xmlStrEqual(subNode->name, LABEL))
            {
                xmlChar *key = xmlNodeListGetString(doc,
                                    subNode->xmlChildrenNode, 1);
                gwy_spectra_set_title(spectra, strdup((gchar *)key));
                xmlFree(key);
            }
            else if (xmlStrEqual(subNode->name, DATA_POINTS))
            {
                xmlChar *key = xmlNodeListGetString(doc,
                                    subNode->xmlChildrenNode, 1);
                numDataPoints = (guint32)atoi((char *)key);
                xmlFree(key);
            }
            else if (xmlStrEqual(subNode->name, START_WAVENUMBER))
            {
                xmlChar *key = xmlNodeListGetString(doc,
                                    subNode->xmlChildrenNode, 1);
                startWavenum = (gdouble)atof((char *)key);
                xmlFree(key);
            }
            else if (xmlStrEqual(subNode->name, END_WAVENUMBER))
            {
                xmlChar *key = xmlNodeListGetString(doc,
                                    subNode->xmlChildrenNode, 1);
                endWavenum = (gdouble)atof((char *)key);
                xmlFree(key);
            }
            else if (xmlStrEqual(subNode->name, LOCATION))
            {
                for (xmlNode *locNode = subNode->children;
                     locNode; locNode = locNode->next)
                {
                    if (locNode->type != XML_ELEMENT_NODE)
                        continue;
                    xmlChar *key = xmlNodeListGetString(doc, locNode->xmlChildrenNode, 1);
                    if (xmlStrEqual(locNode->name, X))
                        location_x = (gdouble)atof((char *)key);
                    else if (xmlStrEqual(locNode->name, Y))
                        location_y = (gdouble)atof((char *)key);
                    xmlFree(key);
                }
            }
            else if (xmlStrEqual(subNode->name, DATA_CHANNELS))
            {
                gwy_spectra_set_spectrum_y_label(spectra,
                    strdup((gchar *)xmlGetProp(subNode, DATA_CHANNEL)));
                for (xmlNode *dcNode = subNode->children;
                     dcNode; dcNode = dcNode->next)
                {
                    if (dcNode->type != XML_ELEMENT_NODE)
                        continue;
                    if (xmlStrEqual(dcNode->name, SAMPLE_BASE_64))
                    {
                        xmlChar *key = xmlNodeListGetString(doc,
                                            dcNode->xmlChildrenNode, 1);
                        base64SpecString = (guchar *)strdup((gchar *)key);
                        xmlFree(key);
                        break;
                    }
                }
            }
        }

        dataline = gwy_data_line_new(numDataPoints,
            (endWavenum-startWavenum)*(1.0+(1.0/((gdouble)numDataPoints-1.0))),
            FALSE);
        gwy_data_line_set_offset(dataline, startWavenum);

        gwy_spectra_add_spectrum(spectra, dataline,
                                 location_x*1.0E-6, location_y*1.0E-6);
        gwy_spectra_add_spectrum(spectra_all, dataline,
                                 location_x*1.0E-6, location_y*1.0E-6);
        gwy_si_unit_set_from_string(gwy_data_line_get_si_unit_x(dataline),
                                    NULL);
        gwy_si_unit_set_from_string(gwy_data_line_get_si_unit_y(dataline),
                                    NULL);

        ydata = gwy_data_line_get_data(dataline);
        decodedData = decodeBase64(base64SpecString);
        readMarker = decodedData;
        for (guint32 k = 0; k < numDataPoints; ++k)
            ydata[k] = gwy_get_gfloat_le(&readMarker);

        g_snprintf(id, sizeof(id), "/sps/%i", specNum);
        gwy_container_set_object_by_name(container, id, spectra);

        g_free(base64SpecString);
    }
    gwy_container_set_object_by_name(container, "/sps/0", spectra_all);
}

static guchar*
decodeBase64(const guchar* input)
{
    /*const guint32 inputStrLength = strlen((gchar *)input);
    const guint32 length = (guint32)((gdouble)inputStrLength*6.0*0.125) + 1;
    guchar* output = (guchar*)malloc(length);
    gchar* c = (gchar *)output;
    gint32 cnt = 0;
    base64_decodestate s;
    base64_init_decodestate(&s);
    cnt = base64_decode_block((gchar *)input, inputStrLength, c, &s);
    c += cnt;
    *c = 0;
    return output;*/
    const guint32 inputStrLength = strlen((gchar *)input);
    const guint32 length = (guint32)((gdouble)inputStrLength*6.0*0.125) + 1;
    gchar* output = malloc(length);
    gchar* c = output;
    gint32 cnt = 0;
    base64_decodestate s;
    base64_init_decodestate(&s);
    cnt = base64_decode_block((gchar *)input, inputStrLength, c, &s);
    c += cnt;
    *c = 0;
    return (guchar *)output;
}