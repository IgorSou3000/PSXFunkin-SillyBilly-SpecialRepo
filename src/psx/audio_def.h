#pragma once

#define XA_LENGTH(x) (((u64)(x) * 75) / 100 * IO_SECT_SIZE) //Centiseconds to sectors in bytes (w)

typedef struct
{
	XA_File file;
	u32 length;
} XA_TrackDef;

static const XA_TrackDef xa_tracks[] = {
	//MENU.XA
	{XA_Menu, XA_LENGTH(11295)}, //XA_GettinFreaky
	{XA_Menu, XA_LENGTH(3840)},  //XA_GameOver
	//WEEK1A.XA
	{XA_Week1A, XA_LENGTH(37400)}, //XA_SillyBilly
};

static const char *xa_paths[] = {
	"\\SONGS\\MENU.MUS;1",   //XA_Menu
	"\\SONGS\\WEEK1A.MUS;1", //XA_Week1A
	NULL,
};
