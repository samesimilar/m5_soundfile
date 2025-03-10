

# change this to match the name of the external you would like to generate
# (do not add a ~ suffix to the name here)
lib.name = m5_soundfile



# pd.src = /Users/samesimilar/rep/pd-0.55-2/src

$(lib.name).class.sources = m5_soundfile.c m5_soundfile_wave.c m5_timeanchor.c
# cflags = -I$(pd.src)
# cflags = -DDEBUG_READ_LOOP -DDEBUG_SOUNDFILE_THREADS 
# cflags = -DDEBUG_SOUNDFILE_THREADS
# cflags = -DDEBUG_READ_LOOP
suppress-wunused = yes

define forDarwin
  cflags += -mmacosx-version-min=10.9 -DHAVE_UNISTD_H 
endef

define forLinux
  cflags += -DHAVE_UNISTD_H 
endef


# datafiles =
# datafiles and datadirs:
# All extra files you want to include in binary distributions of the
# library: abstractions and help patches, example patches, meta patch, readme
# and license texts, manuals, sound files, etcetera. Use 'datafiles' for all
# files that should go into your lib rootdir and 'datadirs' for complete
# directories you want to copy from source to distribution.

include ./pd-lib-builder/Makefile.pdlibbuilder