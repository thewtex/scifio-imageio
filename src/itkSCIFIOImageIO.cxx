/*=========================================================================
 *
 *  Copyright Insight Software Consortium
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0.txt
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *=========================================================================*/
#include <fstream>

#include "itkSCIFIOImageIO.h"
#include "itkIOCommon.h"
#include "itkExceptionObject.h"
#include "itkMetaDataObject.h"

#include <cmath>

#include <stdio.h>
#include <stdlib.h>

#if defined (_WIN32)
#define PATHSTEP ';'
#define SLASH '\\'
#else
#define PATHSTEP ':'
#define SLASH '/'
#endif

#ifdef WIN32
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <math.h>

#endif

#define scifioDebug(msg)          \
  {                               \
  std::cout << msg << std::endl;  \
  }

namespace
{
  std::string getEnv( const char* name )
  {
    char* result = getenv(name);
    if ( result == NULL )
      {
      return "";
      }
    return result;
  }
}

namespace itk
{
template <typename ReturnType>
ReturnType valueOfString( const std::string &s )
{
  ReturnType res;
  if( !(std::istringstream(s) >> res) )
    {
    itkGenericExceptionMacro(<<"SCIFIOImageIO: error while converting: " << s );
    }
  return res;
}

template <typename T>
T GetTypedMetaData ( MetaDataDictionary dict, std::string key )
{
  std::string tmp;
  ExposeMetaData<std::string>(dict, key, tmp);
  return valueOfString<T>(tmp);
}

template <>
bool valueOfString <bool> ( const std::string &s )
{
  std::stringstream ss;
  ss << s;
  bool res = false;
  ss >> res;
  if( ss.fail() )
  {
    ss.clear();
    ss >> std::boolalpha >> res;
  }
  return res;
}

template<typename T>
std::string toString( const T & Value )
{
  std::ostringstream oss;
  oss << Value;
  return oss.str();
}

SCIFIOImageIO::SCIFIOImageIO()
{
  scifioDebug("SCIFIOImageIO constructor");

  this->m_FileType = Binary;

  // determine Java classpath from SCIFIO_PATH environment variable
  std::string scifioPath = getEnv("SCIFIO_PATH");
  if( scifioPath == "" )
    {
    itkExceptionMacro("SCIFIO_PATH is not set. " <<
                      "This environment variable must point to the " <<
                      "directory containing the SCIFIO JAR files");
    }
  std::string classpath = scifioPath + "/*";

  // determine path to java executable from JAVA_HOME, if available
  std::string javaCmd = "java";
  std::string javaHome = getEnv("JAVA_HOME");
  if( javaHome == "" )
    {
    scifioDebug("Warning: JAVA_HOME not set; assuming Java is on the path");
    }
  else
    {
    std::vector<std::string> javaCmdPath;
    javaCmdPath.push_back( "" ); // NB: JoinPath skips the first one (why?).
    javaCmdPath.push_back( javaHome );
    javaCmdPath.push_back( "bin" );
    javaCmdPath.push_back( "java" );
    javaCmd = itksys::SystemTools::JoinPath(javaCmdPath);
    }

  m_Args.push_back( javaCmd );
  // TODO: Parse memory settings and other flags from environment variables.
  m_Args.push_back( "-Xmx256m" );
  m_Args.push_back( "-Djava.awt.headless=true" );
  m_Args.push_back( "-cp" );
  m_Args.push_back( classpath );
  // NB: The package "loci.formats" will change to "ome.scifio" in a future
  // release. When SCIFIO is updated to a 4.5.x version, this string will
  // likely need to change to something like "ome.scifio.itk.SCIFIOImageIO".
  m_Args.push_back( "loci.formats.itk.ITKBridgePipes" );
  m_Args.push_back( "waitForInput" );

  // output full Java command, for debugging
  scifioDebug("-- JAVA COMMAND --");
  for (unsigned int i=0; i<m_Args.size(); i++) {
   scifioDebug("\t" << m_Args.at(i));
  }

  // convert to something usable by itksys
  m_Argv = toCArray( m_Args );
  m_Process = NULL;
}

void SCIFIOImageIO::CreateJavaProcess()
{
  if( m_Process )
    {
    // process is still there
    if( itksysProcess_GetState( m_Process ) == itksysProcess_State_Executing )
      {
      // already created and running - just return
      return;
      }
    else
      {
      // still there but not running.
      // destroy it cleanly and continue with the creation process
      DestroyJavaProcess();
      }
    }

#ifdef WIN32
   SECURITY_ATTRIBUTES saAttr;
   saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
   saAttr.bInheritHandle = TRUE;
   saAttr.lpSecurityDescriptor = NULL;

  if( !CreatePipe( &(m_Pipe[0]), &(m_Pipe[1]), &saAttr, 0) )
    itkExceptionMacro(<<"createpipe() failed");
  if ( ! SetHandleInformation(m_Pipe[1], HANDLE_FLAG_INHERIT, 0) )
    itkExceptionMacro(<<"set inherited failed");
#else
  pipe( m_Pipe );
#endif

  m_Process = itksysProcess_New();
  itksysProcess_SetCommand( m_Process, m_Argv );
  itksysProcess_SetPipeNative( m_Process, itksysProcess_Pipe_STDIN, m_Pipe);

  itksysProcess_Execute( m_Process );

  int state = itksysProcess_GetState( m_Process );
  switch( state )
    {
    case itksysProcess_State_Exited:
      {
      int retCode = itksysProcess_GetExitValue( m_Process );
      itkExceptionMacro(<<"SCIFIOImageIO: ITKReadImageInformation exited with return value: " << retCode);
      break;
      }
    case itksysProcess_State_Error:
      {
      std::string msg = itksysProcess_GetErrorString( m_Process );
      itkExceptionMacro(<<"SCIFIOImageIO: ITKReadImageInformation error:" << std::endl << msg);
      break;
      }
    case itksysProcess_State_Exception:
      {
      std::string msg = itksysProcess_GetExceptionString( m_Process );
      itkExceptionMacro(<<"SCIFIOImageIO: ITKReadImageInformation exception:" << std::endl << msg);
      break;
      }
    case itksysProcess_State_Executing:
      {
      // this is the expected state
      break;
      }
    case itksysProcess_State_Expired:
      {
      itkExceptionMacro(<<"SCIFIOImageIO: internal error: ITKReadImageInformation expired.");
      break;
      }
    case itksysProcess_State_Killed:
      {
      itkExceptionMacro(<<"SCIFIOImageIO: internal error: ITKReadImageInformation killed.");
      break;
      }
    case itksysProcess_State_Disowned:
      {
      itkExceptionMacro(<<"SCIFIOImageIO: internal error: ITKReadImageInformation disowned.");
      break;
      }
//     case kwsysProcess_State_Starting:
//       {
//       break;
//       }
    default:
      {
      itkExceptionMacro(<<"SCIFIOImageIO: internal error: ITKReadImageInformation is in unknown state.");
      break;
      }
    }
}

SCIFIOImageIO::~SCIFIOImageIO()
{
  scifioDebug( "SCIFIOImageIO::~SCIFIOImageIO");
  DestroyJavaProcess();
  delete m_Argv;
}

void SCIFIOImageIO::DestroyJavaProcess()
{
  if( m_Process == NULL )
    {
    // nothing to destroy
    return;
    }

  if( itksysProcess_GetState( m_Process ) == itksysProcess_State_Executing )
    {
    scifioDebug("SCIFIOImageIO::DestroyJavaProcess killing java process");
    itksysProcess_Kill( m_Process );
    itksysProcess_WaitForExit( m_Process, NULL );
    }

  scifioDebug("SCIFIOImageIO::DestroyJavaProcess destroying java process");
  itksysProcess_Delete( m_Process );
  m_Process = NULL;

#ifdef WIN32
  CloseHandle( m_Pipe[1] );
#else
  close( m_Pipe[1] );
#endif
}

bool SCIFIOImageIO::CanReadFile( const char* FileNameToRead )
{
  scifioDebug( "SCIFIOImageIO::CanReadFile: FileNameToRead = " << FileNameToRead);

  CreateJavaProcess();

  // send the command to the java process
  std::string command = "canRead\t";
  command += FileNameToRead;
  command += "\n";
  scifioDebug("SCIFIOImageIO::CanRead command: " << command);



#ifdef WIN32
  DWORD bytesWritten;
  bool r = WriteFile( m_Pipe[1], command.c_str(), command.size(), &bytesWritten, NULL );
#else
  write( m_Pipe[1], command.c_str(), command.size() );
#endif

  // fflush( m_Pipe[1] );

  // and read its reply
  std::string imgInfo;
  std::string errorMessage;
  char * pipedata;
  int pipedatalength = 1000;

  bool keepReading = true;
  while( keepReading )
    {
    int retcode = itksysProcess_WaitForData( m_Process, &pipedata, &pipedatalength, NULL );
    // scifioDebug( "SCIFIOImageIO::ReadImageInformation: reading " << pipedatalength << " bytes.");
    if( retcode == itksysProcess_Pipe_STDOUT )
      {

      imgInfo += std::string( pipedata, pipedatalength );
      // if the two last char are "\n\n", then we're done
#ifdef WIN32
      if( imgInfo.size() >= 4 && imgInfo.substr( imgInfo.size()-4, 4 ) == "\r\n\r\n" )
#else
      if( imgInfo.size() >= 2 && imgInfo.substr( imgInfo.size()-2, 2 ) == "\n\n" )
#endif
        {
        keepReading = false;
        }
      }
    else if( retcode == itksysProcess_Pipe_STDERR )
      {
      errorMessage += std::string( pipedata, pipedatalength );
      }
    else
      {
      DestroyJavaProcess();
      itkExceptionMacro("SCIFIOImageIO: 'ITKBridgePipes canRead' exited abnormally. " << errorMessage);
      }
    }
  scifioDebug("SCIFIOImageIO::CanRead error output: " << errorMessage);

  // we have one thing per line
  int p0 = 0;
  int p1 = 0;
  std::string canRead;
  // can read?
  p1 = imgInfo.find("\n", p0);
  canRead = imgInfo.substr( p0, p1 );

  return valueOfString<bool>(canRead);
}

void SCIFIOImageIO::ReadImageInformation()
{

  scifioDebug( "SCIFIOImageIO::ReadImageInformation: m_FileName = " << m_FileName);

  CreateJavaProcess();

  // send the command to the java process
  std::string command = "info\t";
  command += m_FileName;
  command += "\n";
  scifioDebug("SCIFIOImageIO::ReadImageInformation command: " << command);



#ifdef WIN32
  DWORD bytesWritten;
  WriteFile( m_Pipe[1], command.c_str(), command.size(), &bytesWritten, NULL );
#else
  write( m_Pipe[1], command.c_str(), command.size() );
#endif

  // fflush( m_Pipe[1] );
  std::string imgInfo;
  std::string errorMessage;
  char * pipedata;
  int pipedatalength = 1000;

  bool keepReading = true;
  while( keepReading )
    {
    int retcode = itksysProcess_WaitForData( m_Process, &pipedata, &pipedatalength, NULL );
    //scifioDebug( "SCIFIOImageIO::ReadImageInformation: reading " << pipedatalength << " bytes.");


    if( retcode == itksysProcess_Pipe_STDOUT )
      {
      imgInfo += std::string( pipedata, pipedatalength );
      // if the two last char are "\n\n", then we're done
#ifdef WIN32
        if( imgInfo.size() >= 4 && imgInfo.substr( imgInfo.size()-4, 4 ) == "\r\n\r\n" )
#else
        if( imgInfo.size() >= 2 && imgInfo.substr( imgInfo.size()-2, 2 ) == "\n\n" )
#endif
        {
        keepReading = false;
        }
      }
    else if( retcode == itksysProcess_Pipe_STDERR )
      {
      errorMessage += std::string( pipedata, pipedatalength );
      }
    else
      {
      DestroyJavaProcess();
      itkExceptionMacro("SCIFIOImageIO: 'ITKBridgePipes info' exited abnormally. " << errorMessage);
      }
    }

  scifioDebug("SCIFIOImageIO::ReadImageInformation error output: " << errorMessage);

  this->SetNumberOfDimensions(5);

  // fill the metadata dictionary
  MetaDataDictionary & dict = this->GetMetaDataDictionary();

  // we have one thing per two lines
  size_t p0 = 0;
  size_t p1 = 0;
  std::string line;

  while( p0 < imgInfo.size() )
    {

    // get the key line
#ifdef WIN32
    p1 = imgInfo.find("\r\n", p0);
#else
    p1 = imgInfo.find("\n", p0);
#endif

    line = imgInfo.substr( p0, p1-p0 );

    // ignore the empty lines
    if( line == "" )
      {
      // go to the next line
#ifdef WIN32
      p0 = p1+2;
#else
      p0 = p1+1;
#endif
      continue;
      }

    std::string key = line;
    // go to the next line
#ifdef WIN32
      p0 = p1+2;
#else
      p0 = p1+1;
#endif

    // get the value line
#ifdef WIN32
    p1 = imgInfo.find("\r\n", p0);
#else
    p1 = imgInfo.find("\n", p0);
#endif

    line = imgInfo.substr( p0, p1-p0 );

    // ignore the empty lines
#ifdef WIN32
    if( line == "\r" )
#else
    if( line == "" )
#endif
      {
      // go to the next line
#ifdef WIN32
      p0 = p1+2;
#else
      p0 = p1+1;
#endif
      continue;
      }

    std::string value = line;
    //scifioDebug("=== " << key << " = " << value << " ===");

    // store the values in the dictionary
    if( dict.HasKey(key) )
      {
      scifioDebug("SCIFIOImageIO::ReadImageInformation metadata " << key << " = " << value << " ignored because the key is already defined.");
      }
    else
      {
        std::string tmp;
        // we have to unescape \\ and \n
        size_t lp0 = 0;
        size_t lp1 = 0;

        while( lp0 < value.size() )
          {
          lp1 = value.find( "\\", lp0 );
          if( lp1 == std::string::npos )
            {
            tmp += value.substr( lp0, value.size()-lp0 );
            lp0 = value.size();

            }
          else
            {
            tmp += value.substr( lp0, lp1-lp0 );
            if( lp1 < value.size() - 1 )
              {
              if( value[lp1+1] == '\\' )
                {
                tmp += '\\';
                }
              else if( value[lp1+1] == 'n' )
                {
                tmp += '\n';
                }
              }
            lp0 = lp1 + 2;
            }

          }
        scifioDebug("Storing metadata: " << key << " ---> " << tmp);
        EncapsulateMetaData< std::string >( dict, key, tmp );
      }

    // go to the next line
#ifdef WIN32
      p0 = p1+2;
#else
      p0 = p1+1;
#endif

    }

  // save the dicitonary

  itkMeta = dict;

  // set the values needed by the reader
  std::string s;
  bool b;
  long i;
  double r;

  // is interleaved?
  b = GetTypedMetaData<bool>(dict, "Interleaved");
  if( b )
    {
    scifioDebug("Interleaved ---> True");
    }
  else
    {
    scifioDebug("Interleaved ---> False");
    }

  // is little endian?
  b = GetTypedMetaData<bool>(dict, "LittleEndian");
  if( b )
    {
    scifioDebug("Setting LittleEndian ---> True");
    SetByteOrderToLittleEndian();
    }
  else
    {
    scifioDebug("Setting LittleEndian ---> False");
    SetByteOrderToBigEndian();
    }

  // component type
  itkAssertOrThrowMacro( dict.HasKey("PixelType"), "PixelType is not in the metadata dictionary!");
  i = GetTypedMetaData<long>(dict, "PixelType");
  if( i == UNKNOWNCOMPONENTTYPE)
    {
    itkExceptionMacro("Unknown pixel type: "<< i);
    }
  scifioDebug("Setting ComponentType: " << i);
  SetComponentType( scifioToTIKComponentType(i) );

  // x, y, z, t, c
  i = GetTypedMetaData<long>(dict, "SizeX");
  scifioDebug("Setting SizeX: " << i);
  this->SetDimensions( 0, i );
  i = GetTypedMetaData<long>(dict, "SizeY");
  scifioDebug("Setting SizeY: " << i);
  this->SetDimensions( 1, i );
  i = GetTypedMetaData<long>(dict, "SizeZ");
  scifioDebug("Setting SizeZ: " << i);
  this->SetDimensions( 2, i );
  i = GetTypedMetaData<long>(dict, "SizeT");
  scifioDebug("Setting SizeT: " << i);
  this->SetDimensions( 3, i );
  i = GetTypedMetaData<long>(dict, "SizeC");
  scifioDebug("Setting SizeC: " << i);
  this->SetDimensions( 4, i );

  // number of components
  i = GetTypedMetaData<long>(dict, "RGBChannelCount");
  if( i == 1 )
    {
    this->SetPixelType( SCALAR );
    }
  else if( i == 3 )
    {
    this->SetPixelType( RGB );
    }
  else
    {
    this->SetPixelType( VECTOR );
    }
  this->SetNumberOfComponents( i );

  // spacing
  r = GetTypedMetaData<double>(dict, "PixelsPhysicalSizeX");
  scifioDebug("Setting PixelsPhysicalSizeX: " << r);
  this->SetSpacing( 0, r );
  r = GetTypedMetaData<double>(dict, "PixelsPhysicalSizeY");
  scifioDebug("Setting PixelsPhysicalSizeY: " << r);
  this->SetSpacing( 1, r );
  r = GetTypedMetaData<double>(dict, "PixelsPhysicalSizeZ");
  scifioDebug("Setting PixelsPhysicalSizeZ: " << r);
  this->SetSpacing( 2, r );
  r = GetTypedMetaData<double>(dict, "PixelsPhysicalSizeT");
  scifioDebug("Setting PixelsPhysicalSizeT: " << r);
  this->SetSpacing( 3, r );
  r = GetTypedMetaData<double>(dict, "PixelsPhysicalSizeC");
  scifioDebug("Setting PixelsPhysicalSizeC: " << r);
  this->SetSpacing( 4, r );
}

void SCIFIOImageIO::Read(void* pData)
{
  scifioDebug("SCIFIOImageIO::Read");
  const ImageIORegion & region = this->GetIORegion();

  CreateJavaProcess();

  scifioDebug("SCIFIOImageIO::Read region: ");


  // send the command to the java process
  std::string command = "read\t";
  command += m_FileName;
  for( unsigned int d=0; d<region.GetImageDimension(); d++ )
    {
    scifioDebug("region index: " << region.GetIndex(d) << "; region size: " << region.GetSize(d));
    command += "\t";
    command += toString(region.GetIndex(d));
    command += "\t";
    command += toString(region.GetSize(d));
    }
  for( int d=region.GetImageDimension(); d<5; d++ )
    {
    command += "\t0\t1";
    }
  command += "\n";
  scifioDebug("SCIFIOImageIO::Read command: " << command);

#ifdef WIN32
  DWORD bytesWritten;
  WriteFile( m_Pipe[1], command.c_str(), command.size(), &bytesWritten, NULL );
#else
  write( m_Pipe[1], command.c_str(), command.size() );
#endif

  // fflush( m_Pipe[1] );

  // and read the image
  char * data = (char *)pData;
  size_t pos = 0;
  std::string errorMessage;
  char * pipedata;
  int pipedatalength;

  size_t byteCount = this->GetPixelSize() * region.GetNumberOfPixels();
  while( pos < byteCount )
    {
    int retcode = itksysProcess_WaitForData( m_Process, &pipedata, &pipedatalength, NULL );
    // scifioDebug( "SCIFIOImageIO::ReadImageInformation: reading " << pipedatalength << " bytes.");
    if( retcode == itksysProcess_Pipe_STDOUT )
      {
      // std::cout << "pos: " << pos << "  reading: " << pipedatalength << std::endl;
      memcpy( data + pos, pipedata, pipedatalength );
      pos += pipedatalength;
      }
    else if( retcode == itksysProcess_Pipe_STDERR )
      {
      errorMessage += std::string( pipedata, pipedatalength );
      }
    else
      {
      DestroyJavaProcess();
      itkExceptionMacro(<<"SCIFIOImageIO: 'ITKBridgePipes read' exited abnormally. " << errorMessage);
      }
    }

  scifioDebug("SCIFIOImageIO::Read error output: " << errorMessage);
}

bool SCIFIOImageIO::CanWriteFile(const char* name)
{
  scifioDebug("SCIFIOImageIO::CanWriteFile: name = " << name);
  CreateJavaProcess();

  std::string command = "canWrite\t";
  command += name;
  command += "\n";

#ifdef WIN32
 DWORD bytesWritten;
  WriteFile( m_Pipe[1], command.c_str(), command.size(), &bytesWritten, NULL );
#else
  write( m_Pipe[1], command.c_str(), command.size() );
#endif

  std::string imgInfo;
  std::string errorMessage;
  char * pipedata;
  int pipedatalength = 1000;

  bool keepReading = true;
  while( keepReading )
    {
    int retcode = itksysProcess_WaitForData( m_Process, &pipedata, &pipedatalength, NULL );
    scifioDebug( "SCIFIOImageIO::CanWriteFile: reading " << pipedatalength << " bytes.");
    if( retcode == itksysProcess_Pipe_STDOUT )
      {
      imgInfo += std::string( pipedata, pipedatalength );
      // if the two last char are "\n\n", then we're done
#ifdef WIN32
      if( imgInfo.size() >= 4 && imgInfo.substr( imgInfo.size()-4, 4 ) == "\r\n\r\n" )
#else
      if( imgInfo.size() >= 2 && imgInfo.substr( imgInfo.size()-2, 2 ) == "\n\n" )
#endif
        {
        keepReading = false;
        }
      }
    else if( retcode == itksysProcess_Pipe_STDERR )
      {
      errorMessage += std::string( pipedata, pipedatalength );
      }
    else
      {
      DestroyJavaProcess();
      itkExceptionMacro(<<"SCIFIOImageIO: 'ITKBridgePipes canWrite' exited abnormally. " << errorMessage);
      }
    }

  scifioDebug("SCIFIOImageIO::CanWrite error output: " << errorMessage);

  // we have one thing per line
  int p0 = 0;
  int p1 = 0;
  std::string canWrite;
  // can write?
  p1 = imgInfo.find("\n", p0);
  canWrite = imgInfo.substr( p0, p1 );
  scifioDebug("CanWrite result: " << canWrite);
  return valueOfString<bool>(canWrite);
}

void SCIFIOImageIO::WriteImageInformation()
{
  scifioDebug("SCIFIOImageIO::WriteImageInformation");
  // NB: Nothing to do.
}

void SCIFIOImageIO::Write(const void * buffer )
{
  scifioDebug("SCIFIOImageIO::Write");

  CreateJavaProcess();

  ImageIORegion region = GetIORegion();
  int regionDim = region.GetImageDimension();

  std::string command = "write\t";
  scifioDebug("File name: " << m_FileName);
  command += m_FileName;
  command += "\t";
  scifioDebug("Byte Order: " << GetByteOrderAsString(GetByteOrder()));
  switch(GetByteOrder()) {
    case BigEndian:
      command += toString(1);
      break;
    case LittleEndian:
    default:
      command += toString(0);
  }
  command += "\t";
  scifioDebug("Region dimensions: " << regionDim);
  command += toString(regionDim);
  command += "\t";

  for(int i = 0; i < regionDim; i++){
    scifioDebug("Dimension " << i << ": " << region.GetSize(i));
    command += toString(region.GetSize(i));
    command += "\t";
  }

  for(int i = regionDim; i < 5; i++) {
    scifioDebug("Dimension " << i << ": " << 1);
    command += toString(1);
    command += "\t";
  }

  for(int i = 0; i < regionDim; i++){
    scifioDebug("Phys Pixel size " << i << ": " << this->GetSpacing(i));
    command += toString(this->GetSpacing(i));
    command += "\t";
  }

  for(int i = regionDim; i < 5; i++) {
    scifioDebug("Phys Pixel size" << i << ": " << 1);
    command += toString(1);
    command += "\t";
  }

  scifioDebug("Pixel Type: " << itkToSCIFIOPixelType(GetComponentType()));
  command += toString(itkToSCIFIOPixelType(GetComponentType()));
  command += "\t";

  int rgbChannelCount = GetNumberOfComponents();

  scifioDebug("RGB Channels: " << rgbChannelCount);
  command += toString(rgbChannelCount);
  command += "\t";

  // int xIndex = 0, yIndex = 1
  int zIndex = 2, cIndex = 3, tIndex = 4;
  int bytesPerPlane = rgbChannelCount;
  int numPlanes = 1;

  for (int dim = 0; dim < 5; dim++)
  {
    if(dim < regionDim)
    {
      int index = region.GetIndex(dim);
      int size = region.GetSize(dim);
      scifioDebug("dim = " << dim << " index = " << toString(index) << " size = " << toString(size));
      command += toString(index);
      command += "\t";
      command += toString(size);
      command += "\t";

      if( dim == cIndex || dim == zIndex || dim == tIndex )
      {
        numPlanes *= size - index;
      }
    }
    else
    {
      scifioDebug("dim = " << dim << " index = " << 0 << " size = " << 1);
      command += toString(0);
      command += "\t";
      command += toString(1);
      command += "\t";
    }
  }

  // build lut if necessary

  MetaDataDictionary & dict = itkMeta;

  bool useLut = GetTypedMetaData<bool>(dict, "UseLUT");

  scifioDebug("useLUT = " << useLut);

  if (useLut) {
    command += toString(1);
    command += "\t";
    scifioDebug(""<<command);
    int LUTBits = GetTypedMetaData<int>(dict, "LUTBits");
    command += toString(LUTBits);
    command += "\t";
    scifioDebug(""<<command);
    int LUTLength = GetTypedMetaData<int>(dict, "LUTLength");
    command += toString(LUTLength);
    command += "\t";
    scifioDebug(""<<command);


    scifioDebug("Found a LUT of length: " << LUTLength);
    scifioDebug("Found a LUT of bits: " << LUTBits);

    for(int i = 0; i < LUTLength; i++) {
      if(LUTBits == 8) {
        int rValue = GetTypedMetaData<int>(dict, "LUTR" + toString(i));
        command += toString(rValue);
        command += "\t";
        int gValue = GetTypedMetaData<int>(dict, "LUTG" + toString(i));
        command += toString(gValue);
        command += "\t";
        int bValue = GetTypedMetaData<int>(dict, "LUTB" + toString(i));
        command += toString(bValue);
        command += "\t";
        scifioDebug("Retrieval " << i << " r,g,b values = " << rValue << "," << gValue << "," << bValue);
      }
      else {
        short rValue = GetTypedMetaData<short>(dict, "LUTR" + toString(i));
        command += toString(rValue);
        command += "\t";
        short gValue = GetTypedMetaData<short>(dict, "LUTG" + toString(i));
        command += toString(gValue);
        command += "\t";
        short bValue = GetTypedMetaData<short>(dict, "LUTB" + toString(i));
        command += toString(bValue);
        command += "\t";
        scifioDebug("Retrieval " << i << " r,g,b values = " << rValue << "," << gValue << "," << bValue);
        command += "\t";
      }
    }

  }
  else {
    command += toString(0);
    command += "\t";
  }

  command += "\n";

  scifioDebug("SCIFIOImageIO::Write command: " << command);

#ifdef WIN32
  DWORD bytesWritten;
  WriteFile( m_Pipe[1], command.c_str(), command.size(), &bytesWritten, NULL );
#else
  write( m_Pipe[1], command.c_str(), command.size() );
#endif

  // need to read back the number of planes and bytes per plane to read from buffer
  std::string imgInfo;
  std::string errorMessage;
  char * pipedata;
  int pipedatalength = 1000;

  scifioDebug("SCIFIOImageIO::Write reading data back ...");
  bool keepReading = true;
  while( keepReading )
    {
    int retcode = itksysProcess_WaitForData( m_Process, &pipedata, &pipedatalength, NULL );
    if( retcode == itksysProcess_Pipe_STDOUT )
      {
      imgInfo += std::string( pipedata, pipedatalength );
      // if the two last char are "\n\n", then we're done
#ifdef WIN32
      if( imgInfo.size() >= 4 && imgInfo.substr( imgInfo.size()-4, 4 ) == "\r\n\r\n" )
#else
      if( imgInfo.size() >= 2 && imgInfo.substr( imgInfo.size()-2, 2 ) == "\n\n" )
#endif
        {
        keepReading = false;
        }
      }
    else if( retcode == itksysProcess_Pipe_STDERR )
      {
      errorMessage += std::string( pipedata, pipedatalength );
      //scifioDebug( "In read back loop. errorMessage: " << imgInfo);
      }
    else
      {
      DestroyJavaProcess();
      itkExceptionMacro(<<"SCIFIOImageIO: 'ITKBridgePipes Write' exited abnormally. " << errorMessage);
      }
    }

  scifioDebug("SCIFIOImageIO::Write error output: " << errorMessage);
  scifioDebug("Read imgInfo: " << imgInfo);

  // bytesPerPlane is the first line
  int p0 = 0;
  int p1 = 0;
  std::string vals;
  p1 = imgInfo.find("\n", p0);
  vals = imgInfo.substr( p0, p1 );

  bytesPerPlane = valueOfString<int>(vals);
  scifioDebug("BPP: " << bytesPerPlane << " numPlanes: " << numPlanes);

  typedef unsigned char BYTE;
  BYTE* data = (BYTE*)buffer;

  int pipelength = 10000;

  for (int i = 0; i < numPlanes; i++)
  {
      int bytesRead = 0;
      while(bytesRead < bytesPerPlane )
      {
        scifioDebug("bytesPerPlane: " << bytesPerPlane << " bytesRead: " << bytesRead << " pipelength: " << pipelength);
        int bytesToRead = ((bytesPerPlane - bytesRead) > pipelength ? pipelength : (bytesPerPlane - bytesRead));

        scifioDebug("Writing " << bytesToRead << " bytes to plane " << i << ".  Bytes read: " << bytesRead);

        #ifdef WIN32
            DWORD bytesWritten;
            WriteFile( m_Pipe[1], data, bytesToRead, &bytesWritten, NULL );
        #else
            write( m_Pipe[1], data, bytesToRead );
        #endif

        data += bytesToRead;
        bytesRead += bytesToRead;

        scifioDebug("Waiting for confirmation of end of plane");

        std::string bytesDone;
        keepReading = true;
        while( keepReading )
          {
          int retcode = itksysProcess_WaitForData( m_Process, &pipedata, &pipedatalength, NULL );
          if( retcode == itksysProcess_Pipe_STDOUT )
            {
            bytesDone += std::string( pipedata, pipedatalength );
            // if the two last char are "\n\n", then we're done
#ifdef WIN32
            if( bytesDone.size() >= 4 && bytesDone.substr( bytesDone.size()-4, 4 ) == "\r\n\r\n" )
#else
            if( bytesDone.size() >= 2 && bytesDone.substr( bytesDone.size()-2, 2 ) == "\n\n" )
#endif
              {
              keepReading = false;
              }
            }
          else if( retcode == itksysProcess_Pipe_STDERR )
            {
            errorMessage += std::string( pipedata, pipedatalength );
            //scifioDebug( "In end of data loop. errorMessage: " << bytesDone);
            }
          else
            {
            DestroyJavaProcess();
            itkExceptionMacro(<<"SCIFIOImageIO: 'ITKBridgePipes Write' exited abnormally. " << errorMessage);
            }
          }

        scifioDebug("SCIFIOImageIO::Write error output: " << errorMessage);
        scifioDebug("Read bytesDone: " << bytesDone);
      }

      std::string planeDone;
      keepReading = true;
      while( keepReading )
        {
        int retcode = itksysProcess_WaitForData( m_Process, &pipedata, &pipedatalength, NULL );
        if( retcode == itksysProcess_Pipe_STDOUT )
          {
          planeDone += std::string( pipedata, pipedatalength );
          // if the two last char are "\n\n", then we're done
#ifdef WIN32
          if( planeDone.size() >= 4 && planeDone.substr( planeDone.size()-4, 4 ) == "\r\n\r\n" )
#else
          if( planeDone.size() >= 2 && planeDone.substr( planeDone.size()-2, 2 ) == "\n\n" )
#endif
            {
            keepReading = false;
            }
          }
        else if( retcode == itksysProcess_Pipe_STDERR )
          {
          errorMessage += std::string( pipedata, pipedatalength );
          //scifioDebug( "In end of plane loop. errorMessage: " << planeDone);
          }
        else
          {
          DestroyJavaProcess();
          itkExceptionMacro(<<"SCIFIOImageIO: 'ITKBridgePipes Write' exited abnormally. " << errorMessage);
          }
        }

       scifioDebug("SCIFIOImageIO::Write error output: " << errorMessage);
       scifioDebug("Read planeDone: " << planeDone);
  }
}
} // end namespace itk
