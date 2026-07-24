!IFNDEF CC
CC=cl
!ENDIF

!IFNDEF RC
RC=rc
!ENDIF

CFLAGS=/nologo /DUNICODE /D_UNICODE /W4 /EHsc
LDFLAGS=/nologo
LIBS=user32.lib gdi32.lib comdlg32.lib comctl32.lib shell32.lib dwmapi.lib uxtheme.lib

OBJS=retropad.obj file_io.obj retropad.res

all: retropad.exe

retropad.exe: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) retropad.obj file_io.obj retropad.res $(LIBS) /Fe:$@

retropad.obj: retropad.c resource.h file_io.h
	$(CC) $(CFLAGS) /c retropad.c

file_io.obj: file_io.c file_io.h resource.h
	$(CC) $(CFLAGS) /c file_io.c

retropad.res: retropad.rc resource.h res\retropad.ico
	$(RC) /fo retropad.res retropad.rc

clean:
	-del /q retropad.exe retropad.obj file_io.obj retropad.res 2> NUL
