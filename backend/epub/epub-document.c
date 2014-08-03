/* this file is part of atril, a mate document viewer
 *
 *  Copyright (C) 2014 Avishkar Gupta
 *
 *  Author:
 *   Avishkar Gupta <avishkar.gupta.delhi@gmail.com>
 *
 * Atril is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Atril is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "epub-document.h"
#include "ev-file-helpers.h"
#include "unzip.h"
#include "ev-document-thumbnails.h"
#include "ev-document-find.h"
#include "ev-document-links.h"
#include "ev-document-misc.h"
#include <libxml/parser.h>
#include <libxml/xmlmemory.h>

#include <config.h>

#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include <gtk/gtk.h>
#include <stdio.h>

/*For strcasestr(),strstr()*/
#include <string.h>

typedef enum _xmlParseReturnType 
{
    XML_ATTRIBUTE,
    XML_KEYWORD
}xmlParseReturnType;

typedef struct _contentListNode {  
    gchar* key ;
    gchar* value ;
	gint index ;
}contentListNode;

typedef struct _linknode {
    gchar *pagelink;
    gchar *linktext;
	guint page;
}linknode;

typedef struct _EpubDocumentClass EpubDocumentClass;

struct _EpubDocumentClass
{
    EvDocumentClass parent_class;
};

struct _EpubDocument
{
    EvDocument parent_instance;
	/*Stores the path to the source archive*/
    gchar* archivename ;
	/*Stores the path of the directory where we unzipped the epub*/
    gchar* tmp_archive_dir ;
	/*Stores the contentlist in a sorted manner*/
    GList* contentList ;
    /* A variable to hold our epubDocument for unzipping*/
    unzFile epubDocument ;
	/*The (sub)directory that actually houses the document*/
	gchar* documentdir;
	/*Stores the table of contents*/
	GList *index;
	/*Document title, for the sidebar links*/
	gchar *docTitle;
};

static void       epub_document_document_thumbnails_iface_init (EvDocumentThumbnailsInterface *iface);
static void       epub_document_document_find_iface_init       (EvDocumentFindInterface       *iface); 
static void       epub_document_document_links_iface_init      (EvDocumentLinksInterface      *iface);

EV_BACKEND_REGISTER_WITH_CODE (EpubDocument, epub_document,
	{
		EV_BACKEND_IMPLEMENT_INTERFACE (EV_TYPE_DOCUMENT_THUMBNAILS,
						epub_document_document_thumbnails_iface_init);
		 EV_BACKEND_IMPLEMENT_INTERFACE (EV_TYPE_DOCUMENT_FIND,
								 epub_document_document_find_iface_init);
        EV_BACKEND_IMPLEMENT_INTERFACE (EV_TYPE_DOCUMENT_LINKS,
                                 epub_document_document_links_iface_init);

	} );

static void
epub_document_thumbnails_get_dimensions (EvDocumentThumbnails *document,
                                         EvRenderContext      *rc,
                                         gint                 *width,
                                         gint                 *height)
{
	gdouble page_width, page_height;
	
	page_width = 800;
	page_height = 1080;
	
	*width = MAX ((gint)(page_width * rc->scale + 0.5), 1);
	*height = MAX ((gint)(page_height * rc->scale + 0.5), 1);
}

static GdkPixbuf *
epub_document_thumbnails_get_thumbnail (EvDocumentThumbnails *document,
                                        EvRenderContext      *rc,
                                        gboolean              border)
{
	cairo_surface_t *webpage;
	GdkPixbuf *thumbnailpix = NULL ;
	gint width,height;
	epub_document_thumbnails_get_dimensions(document,rc,&width,&height);
	webpage = ev_document_misc_surface_rotate_and_scale(rc->page->backend_page,width,height,0);
	thumbnailpix = ev_document_misc_pixbuf_from_surface(webpage);
	return thumbnailpix;
}

static gboolean
epub_document_check_hits(EvDocumentFind *document_find,
                         EvPage         *page,
                         const gchar    *text,
                         gboolean        case_sensitive)
{
	gchar *filepath = g_filename_from_uri((gchar*)page->backend_page,NULL,NULL);
	FILE *fp = fopen(filepath,"r");
	GString *buffer; 
	gchar *found ;
	
	while (!feof(fp)) {
		gchar c;
		gint pos=0;
		buffer = g_string_sized_new (1024);
		
		while ((c = fgetc(fp)) != '\n' && !feof(fp)) {
			g_string_insert_c(buffer,pos++,c);
		}

		g_string_insert_c(buffer,pos,'\0');
		
		if (case_sensitive) {
			if ((found = strstr(buffer->str,text)) != NULL) {
				g_string_free(buffer,TRUE);
				fclose(fp);
				return TRUE;
			}
		}
		else {
			
			if ( (found = strcasestr(buffer->str,text)) != NULL) {
				g_string_free(buffer,TRUE);
				fclose(fp);
				return TRUE;
			}
		}
		g_string_free(buffer,TRUE);
	}
	
	fclose(fp);
	return FALSE;
}

static gboolean
epub_document_links_has_document_links(EvDocumentLinks *document_links)
{
    EpubDocument *epub_document = EPUB_DOCUMENT(document_links);

    g_return_if_fail(EPUB_IS_DOCUMENT(epub_document));

    if (!epub_document->index)
        return FALSE;

    return TRUE;
}


typedef struct _LinksCBStruct {
	GtkTreeModel *model;
	GtkTreeIter  *parent;
}LinksCBStruct;

static void
epub_document_make_tree_entry(linknode* ListData,LinksCBStruct* UserData)
{
	GtkTreeIter tree_iter;
	EvLink *link = NULL;
	gboolean expand;
	char *title_markup;

	//These are all children of the document title, and have no chlidren nodes
	expand = FALSE;

	EvLinkDest *ev_dest = NULL;
	EvLinkAction *ev_action;

	/* We shall use a EV_LINK_DEST_TYPE_PAGE for page links,
	 * and a EV_LINK_DEST_TYPE_HLINK(custom) for refs on a page of type url#label
	 * because we need both dest and page label for this.
	 */

	if (g_strrstr(ListData->pagelink,"#") == NULL) {
		ev_dest = ev_link_dest_new_page(ListData->page);
	}
	else {
		ev_dest = ev_link_dest_new_hlink((gchar*)ListData->pagelink,ListData->page);
	}
	
	ev_action = ev_link_action_new_dest (ev_dest);

	link = ev_link_new((gchar*)ListData->linktext,ev_action);

	gtk_tree_store_append (GTK_TREE_STORE (UserData->model), &tree_iter,(UserData->parent));
	title_markup = g_strdup((gchar*)ListData->linktext);
	
	gtk_tree_store_set (GTK_TREE_STORE (UserData->model), &tree_iter,
			    EV_DOCUMENT_LINKS_COLUMN_MARKUP, title_markup,
			    EV_DOCUMENT_LINKS_COLUMN_LINK, link,
			    EV_DOCUMENT_LINKS_COLUMN_EXPAND, expand,
			    -1);
	
	g_free (title_markup);
	g_object_unref (link);		
}

static GtkTreeModel *
epub_document_links_get_links_model(EvDocumentLinks *document_links) 
{
    GtkTreeModel *model = NULL;

	g_return_val_if_fail (EPUB_IS_DOCUMENT (document_links), NULL);
	
    EpubDocument *epub_document = EPUB_DOCUMENT(document_links);
	
    model = (GtkTreeModel*) gtk_tree_store_new (EV_DOCUMENT_LINKS_COLUMN_NUM_COLUMNS,
                                                G_TYPE_STRING,
                                                G_TYPE_OBJECT,
                                                G_TYPE_BOOLEAN,
                                                G_TYPE_STRING);

	LinksCBStruct linkStruct;
	linkStruct.model = model;
	EvLink *link = ev_link_new(epub_document->docTitle,
	                           ev_link_action_new_dest(ev_link_dest_new_page(0)));
	GtkTreeIter parent;

	linkStruct.parent = &parent;
	
	gtk_tree_store_append (GTK_TREE_STORE (model), &parent,NULL);
		
	gtk_tree_store_set (GTK_TREE_STORE (model), &parent,
			    EV_DOCUMENT_LINKS_COLUMN_MARKUP, epub_document->docTitle,
			    EV_DOCUMENT_LINKS_COLUMN_LINK, link,
			    EV_DOCUMENT_LINKS_COLUMN_EXPAND, TRUE,
			    -1);

	g_object_unref(link);
	
	if (epub_document->index) {
		g_list_foreach (epub_document->index,(GFunc)epub_document_make_tree_entry,&linkStruct);
	}
	
    return model;
}

static void
epub_document_document_thumbnails_iface_init (EvDocumentThumbnailsInterface *iface)
{
	iface->get_thumbnail = epub_document_thumbnails_get_thumbnail;
	iface->get_dimensions = epub_document_thumbnails_get_dimensions;
}

static void
epub_document_document_find_iface_init (EvDocumentFindInterface *iface) 
{
	iface->check_for_hits = epub_document_check_hits;
}

static void
epub_document_document_links_iface_init(EvDocumentLinksInterface *iface)
{
    iface->has_document_links = epub_document_links_has_document_links;
    iface->get_links_model = epub_document_links_get_links_model;  
}

static gboolean
epub_document_save (EvDocument *document,
                    const char *uri,
                    GError    **error)
{
	EpubDocument *epub_document = EPUB_DOCUMENT (document);

	return ev_xfer_uri_simple (epub_document->archivename, uri, error);
}

static int
epub_document_get_n_pages (EvDocument *document)
{   
	EpubDocument *epub_document = EPUB_DOCUMENT (document);

        if (epub_document-> contentList == NULL)
                return 0;
            
	return g_list_length(epub_document->contentList);
}

/**
 * epub_remove_temporary_dir : Removes a directory recursively. 
 * This function is same as comics_remove_temporary_dir
 * Returns:
 *   	0 if it was successfully deleted,
 * 	-1 if an error occurred 		
 */
static int 
epub_remove_temporary_dir (gchar *path_name) 
{
	GDir  *content_dir;
	const gchar *filename;
	gchar *filename_with_path;
	
	if (g_file_test (path_name, G_FILE_TEST_IS_DIR)) {
		content_dir = g_dir_open  (path_name, 0, NULL);
		filename  = g_dir_read_name (content_dir);
		while (filename) {
			filename_with_path = 
				g_build_filename (path_name, 
						  filename, NULL);
			epub_remove_temporary_dir (filename_with_path);
			g_free (filename_with_path);
			filename = g_dir_read_name (content_dir);
		}
		g_dir_close (content_dir);
	}
	/* Note from g_remove() documentation: on Windows, it is in general not 
	 * possible to remove a file that is open to some process, or mapped 
	 * into memory.*/
	return (g_remove (path_name));
}


static gboolean
check_mime_type             (const gchar* uri,
                             GError** error);

static gboolean 
open_xml_document           (const gchar* filename);

static gboolean 
set_xml_root_node           (xmlChar* rootname);

static xmlNodePtr
xml_get_pointer_to_node     (xmlChar* parserfor,
                             xmlChar* attributename,
                             xmlChar* attributevalue);
static void 
xml_parse_children_of_node  (xmlNodePtr parent, 
                             xmlChar* parserfor,
                             xmlChar* attributename,
                             xmlChar* attributevalue);

static gboolean 
xml_check_attribute_value   (xmlNode* node,
                             xmlChar * attributename,
                             xmlChar* attributevalue);

static xmlChar* 
xml_get_data_from_node      (xmlNodePtr node,
                             xmlParseReturnType rettype,
                             xmlChar* attributename);

static void 
xml_free_doc();

static void
free_tree_nodes             (gpointer data);

/*Global variables for XML parsing*/
static xmlDocPtr    xmldocument ;
static xmlNodePtr   xmlroot ;
static xmlNodePtr   xmlretval ;

/*
**Functions to parse the xml files.
**Open a XML document for reading 
*/
static gboolean 
open_xml_document ( const gchar* filename )
{
	xmldocument = xmlParseFile(filename);

	if ( xmldocument == NULL )
	{
		return FALSE ;
	}
	else
	{
		return TRUE ;
	}
}

/**
 *Check if the root value is same as rootname .
 *if supplied rootvalue = NULL ,just set root to rootnode . 
**/
static gboolean 
set_xml_root_node(xmlChar* rootname)
{
	xmlroot = xmlDocGetRootElement(xmldocument);
	
	if (xmlroot == NULL) {

		xmlFreeDoc(xmldocument);	
		return FALSE;
	}

    if ( rootname == NULL )
    {
        return TRUE ;
    }

    if ( !xmlStrcmp(xmlroot->name,rootname))
    {
        return TRUE ;
    }
    else
    {
	   return FALSE;
    }
} 

static xmlNodePtr
xml_get_pointer_to_node(xmlChar* parserfor,
                        xmlChar*  attributename,
                        xmlChar* attributevalue )
{
    xmlNodePtr topchild,children ;

    xmlretval = NULL ;

    if ( !xmlStrcmp( xmlroot->name, parserfor) )
    {
        return xmlroot ;
    }

    topchild = xmlroot->xmlChildrenNode ;

    while ( topchild != NULL )
    {
        if ( !xmlStrcmp(topchild->name,parserfor) )
        {
            if ( xml_check_attribute_value(topchild,attributename,attributevalue) == TRUE )
            {
                 xmlretval = topchild;
                 return xmlretval;     
            }
            else 
            {
                /*No need to parse children node*/
                topchild = topchild->next ;
                continue ;
            }
        }

        xml_parse_children_of_node(topchild , parserfor, attributename, attributevalue) ;

        topchild = topchild->next ;
    }

    return xmlretval ;
}

static void 
xml_parse_children_of_node(xmlNodePtr parent, 
                           xmlChar* parserfor,
                           xmlChar* attributename,
                           xmlChar* attributevalue )
{
    xmlNodePtr child = parent->xmlChildrenNode ;
    
    while ( child != NULL )
    {
        if ( !xmlStrcmp(child->name,parserfor))
        {
            if ( xml_check_attribute_value(child,attributename,attributevalue) == TRUE )
            {
                 xmlretval = child;
                 return ;
            }
            else 
            {
                /*No need to parse children node*/
                child = child->next ;
                continue ;
            }
        }

        /*return already if we have xmlretval set*/
        if ( xmlretval != NULL )
        {
            return ;
        }

        xml_parse_children_of_node(child,parserfor,attributename,attributevalue) ;
        child = child->next ;
    }
}

static void 
xml_free_doc()
{
    xmlFreeDoc(xmldocument);
	xmldocument = NULL;
}

static gboolean 
xml_check_attribute_value(xmlNode* node,
                          xmlChar * attributename,
                          xmlChar* attributevalue)
{
    xmlChar* attributefromfile ;
    if ( attributename == NULL || attributevalue == NULL )
    {
         return TRUE ;     
    }
    else if ( !xmlStrcmp(( attributefromfile = xmlGetProp(node,attributename)),
                           attributevalue) )
    {
        xmlFree(attributefromfile);
        return TRUE ;
    }
    xmlFree(attributefromfile);
    return FALSE ;
}

static xmlChar* 
xml_get_data_from_node(xmlNodePtr node,
                       xmlParseReturnType rettype,
                       xmlChar* attributename)
{
    xmlChar* datastring ;
    if ( rettype == XML_ATTRIBUTE )
       datastring= xmlGetProp(node,attributename);
    else
       datastring= xmlNodeListGetString(xmldocument,node->xmlChildrenNode, 1);

    return datastring;
}

static gboolean
check_mime_type(const gchar* uri,GError** error)
{
    GError * err = NULL ;
    const gchar* mimeFromFile = ev_file_get_mime_type(uri,FALSE,&err);
    
    gchar* mimetypes[] = {"application/epub+zip","application/x-booki+zip"};
    int typecount = 2;
    if ( !mimeFromFile )
    {
        if (err)    {
            g_propagate_error (error, err);
        } 
        else    {
            g_set_error_literal (error,
                         EV_DOCUMENT_ERROR,
                         EV_DOCUMENT_ERROR_INVALID,
                         _("Unknown MIME Type"));
        }
        return FALSE;
    }
    else
    {
        int i=0;
        for (i=0; i < typecount ;i++) {
           if ( g_strcmp0(mimeFromFile, mimetypes[i]) == 0  ) {
                return TRUE;
           }
        }

        /*We didn't find a match*/
        g_set_error_literal (error,
                     EV_DOCUMENT_ERROR,
                     EV_DOCUMENT_ERROR_INVALID,
                     _("Not an ePub document"));

        return FALSE;
    }
}

static gboolean
extract_one_file(EpubDocument* epub_document,GError ** error)
{
    GFile * outfile ;
    gsize writesize = 0;
    GString * gfilepath ;
    unz_file_info64 info ;  
    gchar* directory;
	GString* dir_create;
    GFileOutputStream * outstream ;
    gpointer currentfilename = g_malloc0(512);
    gpointer buffer = g_malloc0(512);
    gchar* createdirnametemp = NULL ;
    gchar* createdirname = NULL;
    if ( unzOpenCurrentFile(epub_document->epubDocument) != UNZ_OK )
    {
            return FALSE ;
    } 
        
    unzGetCurrentFileInfo64(epub_document->epubDocument,&info,currentfilename,512,NULL,0,NULL,0) ;
    directory = g_strrstr(currentfilename,"/") ;

    if ( directory != NULL )
        directory++; 

    gfilepath = g_string_new(epub_document->tmp_archive_dir) ;
    g_string_append_printf(gfilepath,"/%s",(gchar*)currentfilename);

    /*if we encounter a directory, make a directory inside our temporary folder.*/
    if (directory != NULL && *directory == '\0')
    {
        g_mkdir(gfilepath->str,0777);
        unzCloseCurrentFile (epub_document->epubDocument) ;
        g_string_free(gfilepath,TRUE);
        g_free(currentfilename);
        g_free(buffer);
        return TRUE;
    }
    else if (directory != NULL && *directory != '\0' ) {
        gchar* createdir = currentfilename;
        /*Since a substring can't be longer than the parent string, allocating space equal to the parent's size should suffice*/
        createdirname = g_malloc0(strlen(currentfilename));
        /* Add the name of the directory and subdiectories,if any to a buffer and then create it */
        createdirnametemp = createdirname;        
        while ( createdir != directory ) {
            (*createdirnametemp) = (*createdir);
            createdirnametemp++;
            createdir++;
        }
        (*createdirnametemp) = '\0';
		dir_create = g_string_new(epub_document->tmp_archive_dir);
		g_string_append_printf(dir_create,"/%s",createdirname);
        g_mkdir_with_parents(dir_create->str,0777);
		g_string_free(dir_create,TRUE);
    }

    outfile = g_file_new_for_path(gfilepath->str);
    outstream = g_file_create(outfile,G_FILE_CREATE_PRIVATE,NULL,error);
    while ( (writesize = unzReadCurrentFile(epub_document->epubDocument,buffer,512) ) != 0 )
    {
        if ( g_output_stream_write((GOutputStream*)outstream,buffer,writesize,NULL,error) == -1 )
        {
            return FALSE ;
        }
    }
    g_output_stream_close((GOutputStream*)outstream,NULL,error);
    g_object_unref(outfile) ;
    g_object_unref(outstream) ;
   
    unzCloseCurrentFile (epub_document->epubDocument) ;
    g_string_free(gfilepath,TRUE);
    g_free(currentfilename);
    g_free(buffer);
	if ( createdirname != NULL) {
		g_free(createdirname);
	}
	return TRUE;
}

static gboolean 
extract_epub_from_container (const gchar* uri, 
                             EpubDocument *epub_document,
                             GError ** error)
{
    GError* err = NULL ;
    GString * temporary_sub_directory ; 
    epub_document->archivename = g_filename_from_uri(uri,NULL,error);
    gchar* epubfilename ;
    if ( !epub_document->archivename )
    {
         if (err)    {
            g_propagate_error (error, err);
        } 
        else    {
            g_set_error_literal (error,
                         EV_DOCUMENT_ERROR,
                         EV_DOCUMENT_ERROR_INVALID,
                         _("could not retrieve filename"));
        }
        return FALSE ;
    }

    epubfilename = g_strrstr(epub_document->archivename,"/");
    if ( *epubfilename == '/' )
    epubfilename++ ;

    temporary_sub_directory = g_string_new( epubfilename );
    g_string_append(temporary_sub_directory,"XXXXXX") ;

    epub_document->tmp_archive_dir = ev_mkdtemp(temporary_sub_directory->str,error) ;

    if (!epub_document->tmp_archive_dir) {
        return FALSE ;
    }

    g_string_free(temporary_sub_directory,TRUE);

    epub_document->epubDocument = unzOpen64(epub_document->archivename);

    if ( epub_document->epubDocument == NULL )
    {
        if (err)    {
            g_propagate_error (error, err);
        } 
        else    {
            g_set_error_literal (error,
                         EV_DOCUMENT_ERROR,
                         EV_DOCUMENT_ERROR_INVALID,
                         _("could not open archive"));
        }
        return FALSE ;
    }
    if ( unzGoToFirstFile(epub_document->epubDocument) != UNZ_OK )
    {
        if (err) {
            g_propagate_error (error, err);
        } 
        else    {
            g_set_error_literal (error,
                         EV_DOCUMENT_ERROR,
                         EV_DOCUMENT_ERROR_INVALID,
                         _("could not extract archive"));
        }
        return FALSE ;
    }
    while ( TRUE )
    {
        if ( extract_one_file(epub_document,&err) == FALSE )
        {
            if (err) {
                g_propagate_error (error, err);
            } 
            else    {
                g_set_error_literal (error,
                             EV_DOCUMENT_ERROR,
                             EV_DOCUMENT_ERROR_INVALID,
                             _("could not extract archive"));
            }
			return FALSE;
        }   

        if ( unzGoToNextFile(epub_document->epubDocument) == UNZ_END_OF_LIST_OF_FILE )
            break ;
    }

    unzClose(epub_document->epubDocument);
    return TRUE ;
}

static gchar* 
get_uri_to_content(const gchar* uri,GError ** error,EpubDocument *epub_document)
{
	gchar* tmp_archive_dir = epub_document->tmp_archive_dir;
    GError *   err = NULL ; 
    gchar*     containerpath = g_filename_from_uri(uri,NULL,&err);
    GString*   absolutepath ;
    gchar*     content_uri ;
    xmlNodePtr rootfileNode ;
    xmlChar*   relativepath;
	gchar*     directorybuffer = g_malloc0(sizeof(gchar*)*100);
	
    if ( !containerpath )
    {
        if (err) {
            g_propagate_error (error,err);
        } 
        else    {
            g_set_error_literal (error,
                                 EV_DOCUMENT_ERROR,
                                 EV_DOCUMENT_ERROR_INVALID,
                                 _("could not retrieve container file"));
        }
        return NULL ;
    }    

    if ( open_xml_document(containerpath) == FALSE )
    {
        g_set_error_literal(error,
                            EV_DOCUMENT_ERROR,
                            EV_DOCUMENT_ERROR_INVALID,
                            _("could not open container file"));
    
        return NULL ;
    }

    if ( set_xml_root_node((xmlChar*)"container") == FALSE)  {

        g_set_error_literal(error,
                            EV_DOCUMENT_ERROR,
                            EV_DOCUMENT_ERROR_INVALID,
                            _("container file is corrupt"));    
        return NULL ;
    }

    if ( (rootfileNode = xml_get_pointer_to_node((xmlChar*)"rootfile",(xmlChar*)"media-type",(xmlChar*)"application/oebps-package+xml")) == NULL)
    {
        g_set_error_literal(error,
                            EV_DOCUMENT_ERROR,
                            EV_DOCUMENT_ERROR_INVALID,
                            _("epub file is invalid or corrput"));
        return NULL ;
    }
    
    relativepath = xml_get_data_from_node(rootfileNode,XML_ATTRIBUTE,(xmlChar*)"full-path") ;
   if ( relativepath == NULL )
    {
        g_set_error_literal(error,
                            EV_DOCUMENT_ERROR,
                            EV_DOCUMENT_ERROR_INVALID,
                            _("epub file is corrupt,no container"));
        return NULL ;
    }
	absolutepath = g_string_new(tmp_archive_dir);
	gchar* documentfolder = g_strrstr((gchar*)relativepath,"/");
	if (documentfolder != NULL) {
		gchar* copybuffer = (gchar*)relativepath ;
		gchar* writer = directorybuffer;

		while(copybuffer != documentfolder) {
			(*writer) = (*copybuffer);
			writer++;copybuffer++;
		}
		*writer = '\0';
		GString *documentdir = g_string_new(tmp_archive_dir);
		g_string_append_printf(documentdir,"/%s",directorybuffer);
		epub_document->documentdir = g_strdup(documentdir->str);

		g_string_free(documentdir,TRUE);
	}
	else
	{
		epub_document->documentdir = g_strdup(tmp_archive_dir);
	}

    g_string_append_printf(absolutepath,"/%s",relativepath);
    content_uri = g_filename_to_uri(absolutepath->str,NULL,&err);
    if ( !content_uri )  {
    if (err) {
            g_propagate_error (error,err);
        } 
        else    {
            g_set_error_literal (error,
                                 EV_DOCUMENT_ERROR,
                                 EV_DOCUMENT_ERROR_INVALID,
                                 _("could not retrieve container file"));
        }
        return NULL ;
    }
    g_string_free(absolutepath,TRUE);
	g_free(directorybuffer);
	xml_free_doc();
    return content_uri ; 
}

static gboolean
link_present_on_page(const gchar* link,const gchar *page_uri)
{
	if (g_strrstr(link, page_uri)) {
		return TRUE;
	}
	else {
		return FALSE;
	}
}

static GList*
setup_document_content_list(const gchar* content_uri, GError** error,gchar *documentdir,GList *docindex)
{
    GList* newlist = NULL ;
    GError *   err = NULL ; 
    gint indexcounter= 1;
    xmlNodePtr manifest,spine,itemrefptr,itemptr ;
    gboolean errorflag = FALSE;
	GList *indexcopy = docindex,*indexcopyiter = docindex;
    gchar* relativepath ;
    GString* absolutepath = g_string_new(NULL);

    if ( open_xml_document(content_uri) == FALSE )
    {
        g_set_error_literal(error,
                            EV_DOCUMENT_ERROR,
                            EV_DOCUMENT_ERROR_INVALID,
                            _("could not parse content manifest"));
    
        return FALSE ;
    }
    if ( set_xml_root_node((xmlChar*)"package") == FALSE)  {

        g_set_error_literal(error,
                            EV_DOCUMENT_ERROR,
                            EV_DOCUMENT_ERROR_INVALID,
                            _("content file is invalid"));    
        return FALSE ;
    }

    if ( ( spine = xml_get_pointer_to_node((xmlChar*)"spine",NULL,NULL) )== NULL )  
    {
         g_set_error_literal(error,
                            EV_DOCUMENT_ERROR,
                            EV_DOCUMENT_ERROR_INVALID,
                            _("epub file has no spine"));    
        return FALSE ;
    }
    
    if ( ( manifest = xml_get_pointer_to_node((xmlChar*)"manifest",NULL,NULL) )== NULL )  
    {
         g_set_error_literal(error,
                            EV_DOCUMENT_ERROR,
                            EV_DOCUMENT_ERROR_INVALID,
                            _("epub file has no manifest"));    
        return FALSE ;
    }

    xmlretval = NULL ;

    /*Get first instance of itemref from the spine*/
    xml_parse_children_of_node(spine,(xmlChar*)"itemref",NULL,NULL);
    
    if ( xmlretval != NULL )
        itemrefptr = xmlretval ;
    else
    {
        errorflag=TRUE;
    }
    /*Parse the spine for remaining itemrefs*/
    do
    {
		indexcopyiter = indexcopy ;
        /*for the first time that we enter the loop, if errorflag is set we break*/
        if ( errorflag )
        {
            break;
        }
        if ( xmlStrcmp(itemrefptr->name,(xmlChar*)"itemref") == 0)
        {    
            contentListNode* newnode = g_malloc0(sizeof(newnode));    
            newnode->key = (gchar*)xml_get_data_from_node(itemrefptr,XML_ATTRIBUTE,(xmlChar*)"idref");
                   if ( newnode->key == NULL )
            {
                errorflag =TRUE;    
                break;
            }
            xmlretval=NULL ;
            xml_parse_children_of_node(manifest,(xmlChar*)"item",(xmlChar*)"id",(xmlChar*)newnode->key);
            
            if ( xmlretval != NULL )
            {
                itemptr = xmlretval ;
            }
            else
            {
                errorflag=TRUE;
                break;
            }
            relativepath = (gchar*)xml_get_data_from_node(itemptr,XML_ATTRIBUTE,(xmlChar*)"href");
            g_string_assign(absolutepath,documentdir);
            g_string_append_printf(absolutepath,"/%s",relativepath);
            newnode->value = g_filename_to_uri(absolutepath->str,NULL,&err);
            if ( newnode->value == NULL )
            {
                errorflag =TRUE;    
                break;
            }
			
			newnode->index = indexcounter++ ;

			/* NOTE:Because the TOC is not always in a sorted manner, we need to check all remaining pages every time.
			 */
			while (indexcopyiter != NULL) {
				linknode *linkdata = indexcopyiter->data;

				if (link_present_on_page(linkdata->pagelink,newnode->value)) {
					linkdata->page = newnode->index - 1;
					indexcopy = indexcopy->next;
				}
				indexcopyiter = indexcopyiter->next;
			}
			
            newlist = g_list_prepend(newlist,newnode);
        }
        itemrefptr = itemrefptr->next ;
    }
    while ( itemrefptr != NULL );

    if ( errorflag )
    {
        if ( err )
        {
            g_propagate_error(error,err);
        }
        else
        {            
            g_set_error_literal(error,
                                EV_DOCUMENT_ERROR,
                                EV_DOCUMENT_ERROR_INVALID,
                                _("Could not set up document tree for loading, some files missing"));
        }
        /*free any nodes that were set up and return empty*/
        g_string_free(absolutepath,TRUE);
        g_list_free_full(newlist,(GDestroyNotify)free_tree_nodes);
        return NULL ;
    }
	newlist = g_list_reverse(newlist);
    g_string_free(absolutepath,TRUE);
	xml_free_doc();
    return newlist ;

}

/* Callback function to free the contentlist.*/
static void
free_tree_nodes(gpointer data)
{
    contentListNode* dataptr = data ;
    g_free(dataptr->value);
    g_free(dataptr->key);
    g_free(dataptr);
}

static void
free_link_nodes(gpointer data)
{
    linknode* dataptr = data ;
    g_free(dataptr->pagelink);
    g_free(dataptr->linktext);
    g_free(dataptr);
}

static gchar*
get_toc_file_name(gchar *containeruri)
{
	gchar *containerfilename = g_filename_from_uri(containeruri,NULL,NULL);

	open_xml_document(containerfilename);

	set_xml_root_node(NULL);

	xmlNodePtr manifest = xml_get_pointer_to_node((xmlChar*)"manifest",NULL,NULL);
	xmlNodePtr spine = xml_get_pointer_to_node((xmlChar*)"spine",NULL,NULL);

	xmlChar *ncx = xml_get_data_from_node(spine,XML_ATTRIBUTE,(xmlChar*)"toc");
	xmlretval = NULL;
	xml_parse_children_of_node(manifest,(xmlChar*)"item",(xmlChar*)"id",ncx);

	gchar* tocfilename = (gchar*)xml_get_data_from_node(xmlretval,XML_ATTRIBUTE,(xmlChar*)"href");
	xml_free_doc();

	return tocfilename;
}

static GList*
setup_document_index(EpubDocument *epub_document,gchar *containeruri) 
{
    GString *tocpath = g_string_new(epub_document->documentdir);
    gchar *tocfilename = get_toc_file_name(containeruri);
    GList *index = NULL;
    g_string_append_printf (tocpath,"/%s",tocfilename);
    GString *pagelink;
    open_xml_document(tocpath->str);
    g_string_free(tocpath,TRUE);
    set_xml_root_node((xmlChar*)"ncx");

	xmlNodePtr docTitle = xml_get_pointer_to_node((xmlChar*)"docTitle",NULL,NULL);
	xmlretval = NULL;
	xml_parse_children_of_node(docTitle,(xmlChar*)"text",NULL,NULL);

	while (epub_document->docTitle == NULL && xmlretval != NULL) {
		epub_document->docTitle = (gchar*)xml_get_data_from_node(xmlretval,XML_KEYWORD,NULL);
		xmlretval = xmlretval->next;
	}
    xmlNodePtr navMap = xml_get_pointer_to_node((xmlChar*)"navMap",NULL,NULL);
	xmlretval = NULL;
    xml_parse_children_of_node(navMap,(xmlChar*)"navPoint",NULL,NULL);

    xmlNodePtr navPoint = xmlretval;

    do {

        if ( !xmlStrcmp(navPoint->name,(xmlChar*)"navPoint")) {
    		xmlretval = NULL;
    		xml_parse_children_of_node(navPoint,(xmlChar*)"navLabel",NULL,NULL);
    		xmlNodePtr navLabel = xmlretval;
    		xmlretval = NULL;
    		gchar *fragment=NULL,*end=NULL;
    		GString *uri = NULL;

    		xml_parse_children_of_node(navLabel,(xmlChar*)"text",NULL,NULL);
            linknode *newnode = g_new0(linknode,1);
    		newnode->linktext = NULL;
    		while (newnode->linktext == NULL) {
        		newnode->linktext = (gchar*)xml_get_data_from_node(xmlretval,XML_KEYWORD,NULL);
    			xmlretval = xmlretval->next;
    		}
    		xmlretval = NULL;
            xml_parse_children_of_node(navPoint,(xmlChar*)"content",NULL,NULL);
            pagelink = g_string_new(epub_document->documentdir);
            newnode->pagelink = (gchar*)xml_get_data_from_node(xmlretval,XML_ATTRIBUTE,(xmlChar*)"src");
            g_string_append_printf(pagelink,"/%s",newnode->pagelink);
            xmlFree(newnode->pagelink);

            if ((end = g_strrstr(pagelink->str,"#")) != NULL) {
            	fragment = g_strdup(g_strrstr(pagelink->str,"#"));
            	*end = '\0';
            }
            uri = g_string_new(g_filename_to_uri(pagelink->str,NULL,NULL));
     		g_string_free(pagelink,TRUE);
     		       
            if (fragment) {
            	g_string_append(uri,fragment);
            }

            newnode->pagelink = g_strdup(uri->str);
            g_string_free(uri,TRUE);
            index = g_list_prepend(index,newnode);
        }
        
        navPoint = navPoint->next;

    } while(navPoint != NULL);

	xml_free_doc();
    
    return g_list_reverse(index);
}

static EvDocumentInfo*
epub_document_get_info(EvDocument *document)
{
	EpubDocument *epub_document = EPUB_DOCUMENT(document);
	GError *error = NULL ;
	gchar* infofile ;
	xmlNodePtr metanode ;
	GString* buffer ;
	gchar* archive_dir = epub_document->tmp_archive_dir;
	GString* containerpath = g_string_new(epub_document->tmp_archive_dir);
	g_string_append_printf(containerpath,"/META-INF/container.xml");
	gchar* containeruri = g_filename_to_uri(containerpath->str,NULL,&error);
	if ( error )
	{
		return NULL ;
	}
	gchar* uri = get_uri_to_content (containeruri,&error,epub_document);
	if ( error )
	{
		return NULL ;
	}
	EvDocumentInfo* epubinfo = g_new0 (EvDocumentInfo, 1);

	epubinfo->fields_mask = EV_DOCUMENT_INFO_TITLE |
			    EV_DOCUMENT_INFO_FORMAT |
			    EV_DOCUMENT_INFO_AUTHOR |
			    EV_DOCUMENT_INFO_SUBJECT |
			    EV_DOCUMENT_INFO_KEYWORDS |
			    EV_DOCUMENT_INFO_LAYOUT |
			    EV_DOCUMENT_INFO_CREATOR |
			    EV_DOCUMENT_INFO_LINEARIZED |
			    EV_DOCUMENT_INFO_N_PAGES ;

	infofile = g_filename_from_uri(uri,NULL,&error);
	if ( error )
		return epubinfo;
	
	open_xml_document(infofile);

	set_xml_root_node((xmlChar*)"package");

	metanode = xml_get_pointer_to_node((xmlChar*)"title",NULL,NULL);
	if ( metanode == NULL )
	  epubinfo->title = NULL ;
	else
	  epubinfo->title = (char*)xml_get_data_from_node(metanode,XML_KEYWORD,NULL);
	
	metanode = xml_get_pointer_to_node((xmlChar*)"creator",NULL,NULL);
	if ( metanode == NULL )
	  epubinfo->author = g_strdup("unknown");
	else
	  epubinfo->author = (char*)xml_get_data_from_node(metanode,XML_KEYWORD,NULL);

	metanode = xml_get_pointer_to_node((xmlChar*)"subject",NULL,NULL);
	if ( metanode == NULL )
	   epubinfo->subject = g_strdup("unknown");
	else
	   epubinfo->subject = (char*)xml_get_data_from_node(metanode,XML_KEYWORD,NULL);

	buffer = g_string_new((gchar*)xml_get_data_from_node (xmlroot,XML_ATTRIBUTE,(xmlChar*)"version"));
	g_string_prepend(buffer,"epub ");
	epubinfo->format = g_strdup(buffer->str);
	
	/*FIXME: Add more of these as you write the corresponding modules*/
	
	epubinfo->layout = EV_DOCUMENT_LAYOUT_SINGLE_PAGE;

	metanode = xml_get_pointer_to_node((xmlChar*)"publisher",NULL,NULL);
	if ( metanode == NULL )
	   epubinfo->creator = g_strdup("unknown");
	else
	   epubinfo->creator = (char*)xml_get_data_from_node(metanode,XML_KEYWORD,NULL);

	/* number of pages */
	epubinfo->n_pages = epub_document_get_n_pages(document);
	
	/*TODO : Add a function to get date*/
	g_free(uri);
	g_string_free(containerpath,TRUE);
	g_string_free(buffer,TRUE);
	
	if (xmldocument)
		xml_free_doc();
	return epubinfo ;
}

static EvPage*
epub_document_get_page(EvDocument *document,
                       gint index)
{
	EpubDocument *epub_document = EPUB_DOCUMENT(document);
	EvPage* page = ev_page_new(index);
	contentListNode *listptr = g_list_nth_data (epub_document->contentList,index);
	page->backend_page = g_strdup(listptr->value);
	return page ;
}


static gboolean
epub_document_load (EvDocument* document,
                    const char* uri,
                    GError**    error)
{
	EpubDocument *epub_document = EPUB_DOCUMENT(document);
	GError* err = NULL ;
	gchar* containeruri ;
	GString *containerpath ;
	gchar* contentOpfUri ;
	if ( check_mime_type (uri,&err) == FALSE )
	{
		/*Error would've been set by the function*/
		g_propagate_error(error,err);
		return FALSE;
	}

	extract_epub_from_container (uri,epub_document,&err);

	if ( err )
	{
		g_propagate_error( error,err );
		return FALSE;
	}

	/*FIXME : can this be different, ever?*/
	containerpath = g_string_new(epub_document->tmp_archive_dir);
	g_string_append_printf(containerpath,"/META-INF/container.xml");
	containeruri = g_filename_to_uri(containerpath->str,NULL,&err);

	if ( err )
	{
		g_propagate_error(error,err);
		return FALSE;
	}
	contentOpfUri = get_uri_to_content (containeruri,&err,epub_document);

	if ( contentOpfUri == NULL )
	{
		g_propagate_error(error,err);
		return FALSE;
	}
	
	epub_document->index = setup_document_index(epub_document,contentOpfUri);
		
	epub_document->contentList = setup_document_content_list (contentOpfUri,&err,epub_document->documentdir,epub_document->index);
	
	if ( epub_document->contentList == NULL )
	{
		g_propagate_error(error,err);
		return FALSE;
	}

	return TRUE ;
}

static void
epub_document_init (EpubDocument *epub_document)
{
    epub_document->archivename = NULL ;
    epub_document->tmp_archive_dir = NULL ;
    epub_document->contentList = NULL ;
	epub_document->documentdir = NULL;
	epub_document->index = NULL;
	epub_document->docTitle = NULL;
}

static void
epub_document_finalize (GObject *object)
{
	EpubDocument *epub_document = EPUB_DOCUMENT (object);
	
	if (epub_document->epubDocument != NULL) {
		if (epub_remove_temporary_dir (epub_document->tmp_archive_dir) == -1)
			g_warning (_("There was an error deleting “%s”."),
				   epub_document->tmp_archive_dir);
	}
	
	if ( epub_document->contentList ) {
            g_list_free_full(epub_document->contentList,(GDestroyNotify)free_tree_nodes);
			epub_document->contentList = NULL;
	}

	if (epub_document->index) {
		g_list_free_full(epub_document->index,(GDestroyNotify)free_link_nodes);
		epub_document->index = NULL;
	}

	if ( epub_document->tmp_archive_dir) {
		g_free (epub_document->tmp_archive_dir);
		epub_document->tmp_archive_dir = NULL;
	}

	if (epub_document->docTitle) {
		g_free(epub_document->docTitle);
		epub_document->docTitle = NULL;
	}
	if ( epub_document->archivename) {
		g_free (epub_document->archivename);
		epub_document->archivename = NULL;
	}
	if ( epub_document->documentdir) {
		g_free (epub_document->documentdir);
		epub_document->documentdir = NULL;
	}
	G_OBJECT_CLASS (epub_document_parent_class)->finalize (object);
}

static void
epub_document_class_init (EpubDocumentClass *klass)
{
	GObjectClass    *gobject_class = G_OBJECT_CLASS (klass);
	EvDocumentClass *ev_document_class = EV_DOCUMENT_CLASS (klass);
	
	gobject_class->finalize = epub_document_finalize;
	ev_document_class->load = epub_document_load;
	ev_document_class->save = epub_document_save;
	ev_document_class->get_n_pages = epub_document_get_n_pages;
	ev_document_class->get_info = epub_document_get_info; 
	ev_document_class->get_page = epub_document_get_page;
}