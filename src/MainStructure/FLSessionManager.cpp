//
//  FLSessionManager.cpp
//
//  Created by Sarah Denoux on 12/04/13.
//  Copyright (c) 2013 __MyCompanyName__. All rights reserved.
//

#include "FLSessionManager.h"
#include "FLSettings.h"
#include "FLWinSettings.h"
#include "utilities.h"
#include "FLErrorWindow.h"

#define DEFAULTNAME "DefaultName"
#define kMaxSHAFolders 100

/* When creating a new DSP, it is associated to a unique SHA Key calculated from 
 the faust code, the name and the compilation options */
/* This allows to accelerate compilation when reusing the same DSP */

FLSessionManager* FLSessionManager::_sessionManager = 0;

//----------------------CONSTRUCTOR/DESTRUCTOR---------------------------
FLSessionManager::FLSessionManager(const QString& sessionFolder)
{
    fSessionFolder = sessionFolder;
}

FLSessionManager::~FLSessionManager(){}

FLSessionManager* FLSessionManager::_Instance()
{
    return FLSessionManager::_sessionManager;
}

void FLSessionManager::createInstance(const QString homePath)
{
    FLSessionManager::_sessionManager = new FLSessionManager(homePath);
}

void FLSessionManager::deleteInstance()
{
    delete FLSessionManager::_sessionManager;
}

//------------------- Compilation of the faust source ----------------------

/* The compilation is divided into 2 steps : factory creation then instance creation */
/* It is not possible to merge those 2 functions because some audio init is needed in between */

QPair<QString, void*> FLSessionManager::createFactory(const QString& source, FLWinSettings* settings, QString& errorMsg)
{
    //-------Clean Factory Folder if needed
    cleanSHAFolder();
    
    //-------Get Faust Code
    
    QString faustContent = ifUrlToString(source);
    
    //Path is whether the dsp source unmodified or the waveform converted
    QString path("");
    if (isSourceDSPPath(faustContent))
        path = faustContent;
    
    faustContent = ifFileToString(faustContent);
    
    //------Get Name
    
    QString name = ifFileToName(path);
    
    if (name == "")
        name = getDeclareName(faustContent);
    
    if (name == "")
        name = "DefaultName";
    
    //--------Calculation of SHA Key
    
    //-----Extracting compilation Options from general options Or window options
    QString defaultOptions = FLSettings::_Instance()->value("General/Compilation/FaustOptions", "").toString();
    int defaultOptLevel = FLSettings::_Instance()->value("General/Compilation/OptValue", 3).toInt();
    
    QString faustOptions = defaultOptions;
    int optLevel = defaultOptLevel;
    
    if (settings) {
        faustOptions = settings->value("Compilation/FaustOptions", defaultOptions).toString();
        optLevel = settings->value("Compilation/OptValue", defaultOptLevel).toInt();
    }
    
    int argc;
	const char** argv = getFactoryArgv(path, faustOptions, argc);
    string shaKey;
    string err;
    //    EXPAND DSP JUST TO GET SHA KEY
    
    if (expandDSPFromString(name.toStdString(), faustContent.toStdString(), argc, argv, shaKey, err) == "") {
        errorMsg = err.c_str();
        return qMakePair(QString(""), (void*)NULL);
    }
//        shaKey = "8F41F6181694A1B561F33328CF75A82DB5E22934";
//	string organizedOptions = FL_reorganize_compilation_options(faustOptions);
    
    string optvalue = QString::number(optLevel).toStdString();
    
//    string fullShaString = organizedOptions + optvalue + faustContent.toStdString();    
//    string shaKey = FL_generate_sha1(fullShaString);
    
    QString factoryFolder = fSessionFolder + "/SHAFolder/" + shaKey.c_str();
    string irFile = factoryFolder.toStdString() + "/" + shaKey;
    QString faustFile = factoryFolder + "/" + shaKey.c_str() + ".dsp";
    
//      Save source
    QDir newFolder(factoryFolder);
    newFolder.mkdir(factoryFolder);
    
    writeFile(faustFile, faustContent);
    
    string fileToCompile(faustFile.toStdString());
    string nameToCompile(name.toStdString());
    
//---- CreateFactory settings
    
    factorySettings* mySetts = new factorySettings;
    factory* toCompile = new factory;
    string error = "";
    
    QString machineName = "local processing";
    
//------ Additionnal compilation step or options (if set so in settings)
    if (settings) {
        machineName = settings->value("RemoteProcessing/MachineName", machineName).toString();
		QString errMsg;
        
        if(!generateAuxFiles(shaKey.c_str(), settings->value("Path", "").toString(), 
            settings->value("AutomaticExport/Options", "").toString(), shaKey.c_str(), errMsg)) {
			FLErrorWindow::_Instance()->print_Error(QString("Additional Compilation Step : ")+ errMsg);
        }
    }
    
//------ Compile local factory
    if (machineName == "local processing") {
        mySetts->fType = TYPE_LOCAL;
        
        //----Use IR Saving if possible
        if (QFileInfo(irFile.c_str()).exists()) {
			toCompile->fLLVMFactory = readDSPFactoryFromBitcodeFile(irFile, "", optLevel);
        }
        //toCompile->fLLVMFactory = readDSPFactoryFromMachineFile(irFile); // in progress but still does not work reliably for all DSP...
        
        //----Create DSP Factory
        if (toCompile->fLLVMFactory == NULL) {
            
            toCompile->fLLVMFactory = createDSPFactoryFromFile(fileToCompile, argc, argv, "", error, optLevel);
            
            if (settings) {
                settings->setValue("InputNumber", 0);
                settings->setValue("OutputNumber", 0);
            }
            
            if (toCompile->fLLVMFactory) {
                writeDSPFactoryToBitcodeFile(toCompile->fLLVMFactory, irFile);
                //writeDSPFactoryToMachineFile(toCompile->fLLVMFactory, irFile); // in progress but still does not work reliably for all DSP...
                writeDependencies(getDependencies(toCompile->fLLVMFactory), shaKey.c_str());
                if (error != "") {
                    FLErrorWindow::_Instance()->print_Error(error.c_str());
                }
            } else {
                errorMsg = error.c_str();
			    return qMakePair(QString(""), (void*)NULL);
            }
        }
    }
//------ Compile remote factory
    else if (settings) {
#ifdef REMOTE
        mySetts->fType = TYPE_REMOTE;
        
        string ip_server = settings->value("RemoteProcessing/MachineIP", "127.0.0.1").toString().toStdString();
        int port_server = settings->value("RemoteProcessing/MachinePort", 7777).toInt();
        
        toCompile->fRemoteFactory = createRemoteDSPFactoryFromString(name.toStdString(), pathToContent(fileToCompile.c_str()).toStdString(), 
                                        argc, argv, ip_server, port_server, error, optLevel);
        
        if (!toCompile->fRemoteFactory) {
            errorMsg = error.c_str();
            return qMakePair(QString(""), (void*)NULL);
        }
        
        settings->setValue("InputNumber", toCompile->fRemoteFactory->getNumInputs());
        settings->setValue("OutputNumber", toCompile->fRemoteFactory->getNumOutputs());
#endif
    }
    
//#ifdef REMOTE
//    if(settings && settings->value("Release/Enabled", false).toBool()){
//        deleteWinFromServer(settings);
//    }
//#endif
    
//----- Saving params because compilation was successfull (otherwise, we return before)
    mySetts->fFactory = toCompile;
    mySetts->fPath = path;
    mySetts->fName = name;
    
//----- If a post-compilation script option is set : execute it !
    if (settings && settings->value("Script/Options", "").toString() != "") {
        QString erroMsg;
        if (!executeInstruction(settings->value("Script/Options", "").toString(), errorMsg)) {
            FLErrorWindow::_Instance()->print_Error(errorMsg);
        }
    }
    
    return qMakePair(QString(shaKey.c_str()), (void*)(mySetts));
}

dsp* FLSessionManager::createDSP(QPair<QString, void*> factorySetts, const QString& source, FLWinSettings* settings, remoteDSPErrorCallback error_callback, void* error_callback_arg, QString& errorMsg)
{
//----- Decode factory settings ------
    factorySettings* mySetts = (factorySettings*)(factorySetts.second);
    factory* toCompile = mySetts->fFactory;
    
    QString path = mySetts->fPath;
    QString name = mySetts->fName;
    int type = mySetts->fType;
    dsp* compiledDSP = NULL;
    
//----Create Local DSP Instance
    if (type == TYPE_LOCAL) {
        compiledDSP = createDSPInstance(toCompile->fLLVMFactory);
		if (compiledDSP == NULL) {
            errorMsg = "Impossible to compile DSP";
        }
    }
#ifdef REMOTE
//----Create Remote DSP Instance
    else if (settings) {
        //int sampleRate = settings->value("SampleRate", 44100).toInt();
        //int bufferSize = settings->value("BufferSize", 512).toInt();
        int errorToCatch;
        
        // -----------CALCULATE ARGUMENTS------------
        int argc;
        const char** argv = getRemoteInstanceArgv(settings, argc);
        compiledDSP = createRemoteDSPInstance(toCompile->fRemoteFactory, argc, argv, error_callback, error_callback_arg, errorToCatch);
        
        /*
        // Test 
        int argc1 = 0;
        const char* argv1[32];
        int errorToCatch1;
        
        argv1[argc1++] = "--NJ_buffer_size";
        string s1 = settings->value("BufferSize", 512).toString().toStdString();
        argv1[argc1++] = s1.c_str();
        
        argv1[argc1++] = "--NJ_sample_rate";
        string s2 = settings->value("SampleRate", 44100).toString().toStdString();
        argv1[argc1++] = s2.c_str();
        
        remote_audio* audio = createRemoteAudioInstance(toCompile->fRemoteFactory, argc1, argv1, errorToCatch1);
        */
        
        if (compiledDSP == NULL) {
            
            //----- If the factory is seen as already compiled but it disapeared, it has to be recompiled
            if (errorToCatch == ERROR_FACTORY_NOTFOUND) {
                QPair<QString, void*> fS = createFactory(source, settings, errorMsg);
                if (fS.second == NULL) {
                    errorMsg = "Impossible to find and recompile factory";
                    return NULL;
                }
                compiledDSP = createDSP(fS, source, settings, error_callback, error_callback_arg, errorMsg);
            }
        }
        
        if (compiledDSP == NULL) {
            errorMsg = getErrorFromCode(errorToCatch);
        }
    }
#else
	Q_UNUSED(source);
	Q_UNUSED(error_callback);
	Q_UNUSED(error_callback_arg);
#endif
    
    fDSPToFactory[compiledDSP] = mySetts;
    
    //-----Save settings
    if (compiledDSP != NULL && settings) {
        settings->setValue("Path", path);
        settings->setValue("Name", name);
        settings->setValue("SHA", factorySetts.first);
	}
    
//#ifdef REMOTE
    //    if(settings && settings->value("Release/Enabled", false).toBool())
    //        addWinToServer(settings);
//#endif
    return compiledDSP;
}

// Factory and instances are associated not to have to maintain both from the ouside of the session manager
void FLSessionManager::deleteDSPandFactory(dsp* toDeleteDSP)
{
    factorySettings* factoryToDelete = fDSPToFactory[toDeleteDSP];
    fDSPToFactory.remove(toDeleteDSP);
    
    if (factoryToDelete->fType == TYPE_LOCAL) {
        deleteDSPInstance((llvm_dsp*) toDeleteDSP);
        deleteDSPFactory(factoryToDelete->fFactory->fLLVMFactory);
    }
#ifdef REMOTE
    else {
        deleteRemoteDSPInstance((remote_dsp*) toDeleteDSP);
        deleteRemoteDSPFactory(factoryToDelete->fFactory->fRemoteFactory);
    }
#endif
}

//--- Managing Faust Source to obtain a name and a Faust program as a string ---

//Return declare name if there is one in the faust program
QString FLSessionManager::getDeclareName(QString text)
{
    QString returning = "";
    int pos = text.indexOf("declare name");
    
    if (pos != -1) {
        text.remove(0, pos);
        
        pos=text.indexOf("\"");
        if (pos != -1) {
            text.remove(0, pos+1);
        }
        pos = text.indexOf("\"");
        text.remove(pos, text.length()-pos);
        
        returning = text;
    }
    
    return returning;
}

bool FLSessionManager::isSourceDSPPath(const QString& source)
{
    return (QFileInfo(source).exists() && QFileInfo(source).completeSuffix() == "dsp");
}

//For the use of google docs (not finished to implement)
void FLSessionManager::receiveDSP()
{
    QNetworkReply* response = (QNetworkReply*)QObject::sender();
    QByteArray key = response->readAll();
    //bool b = QDesktopServices::openUrl(QUrl("https://docs.google.com/a/grame.fr/document/d/13PkB1Ggxo-pFURPwgbS__WXaqGjaIPN9UA_oirRGh5M"));
}

//For the use of google docs (not finished to implement)
void FLSessionManager::networkError(QNetworkReply::NetworkError)
{
//    QNetworkReply* response = (QNetworkReply*)QObject::sender();
}

QString FLSessionManager::ifFileToName(const QString& source)
{
    return (isSourceDSPPath(source)) ? QFileInfo(source).baseName() : "";
}

//--Transforms DSP file into faust string
QString FLSessionManager::ifFileToString(const QString& source)
{
    return (isSourceDSPPath(source)) ? pathToContent(source) : source;
}

/*not finished to implement*/
/* it is easy to get the file if it is public*/
/* if we edit a file which source is a GDoc does it get open in the navigator ? (for : it's logical ; against : it really changes the behavior)*/
/* we need a file watcher version for the GDoc use case */
/* PROBLEM 1 : receiveDSP is asynchroneous ........ */
/* PROBLEM 2 : The google doc is always being saved so it's always asking for recompilation ........ */
QString FLSessionManager::ifGoogleDocToString(const QString& source)
{
    //In case the text dropped is a web url
    int pos = source.indexOf("docs.google.com");
    QString UrlText(source);
    
    //    Has to be at the beginning, otherwise, it can be a component containing an URL.
    if (pos != -1) {

        QNetworkRequest requete(QUrl("https://docs.google.com/a/grame.fr/document/d/13PkB1Ggxo-pFURPwgbS__WXaqGjaIPN9UA_oirRGh5M/export?format=txt"));
        QNetworkAccessManager *m = new QNetworkAccessManager;
        QNetworkReply * fGetKeyReply = m->get(requete);
        
        connect(fGetKeyReply, SIGNAL(finished()), this, SLOT(receiveDSP()));
        connect(fGetKeyReply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(networkError(QNetworkReply::NetworkError)));
    }
    
    return UrlText;
}

//--Transforms an Url into faust string
QString FLSessionManager::ifUrlToString(const QString& source)
{
    //In case the text dropped is a web url
    int pos = source.indexOf("http://");
    
    QString UrlText(source);
    
    //    Has to be at the beginning, otherwise, it can be a component containing an URL.
    if (pos == 0) {
        UrlText = "process = component(\"";
        UrlText += source;
        UrlText +="\");";
    }
    
    return UrlText;
}

//--------- Fill default arguments for local and remote compilation ----------

//--Local params
const char** FLSessionManager::getFactoryArgv(const QString& sourcePath, const QString& faustOptions, int& argc)
{
    //--------Compilation Options 
    
    int numberFixedParams = 4;
    
    if (sourcePath == "")
        numberFixedParams = numberFixedParams-2;

    int iteratorParams = 0;
    
#ifdef _WIN32
    numberFixedParams = numberFixedParams+2;
#endif
    
    //+7 = -I libraryPath -I currentFolder
    argc = numberFixedParams;
    argc += get_numberParameters(faustOptions);
    
    //argc += 1;
    
    const char** argv = new const char*[argc];
    
    //argv[iteratorParams] = "-machine";
    //iteratorParams++;
    
    argv[iteratorParams] = "-I";
    iteratorParams++;
    
    //The library path is where libraries like the scheduler architecture file are = currentSession
    string libsFolder = fSessionFolder.toStdString() + "/Libs";
    
    string libPath = libsFolder;
    char* libP = new char[libsFolder.size()+1];
    strncpy( libP, libPath.c_str(), libsFolder.size()+1);
    argv[iteratorParams] = (const char*) libP;
    iteratorParams++;

    if (sourcePath != "") {
        argv[iteratorParams] = "-I";   
        iteratorParams++;
        
        QString sourceChemin = QFileInfo(sourcePath).absolutePath();
        string path = sourceChemin.toStdString();
        
        char* libP2 = new char[sourceChemin.size()+1];
        strncpy( libP2, path.c_str(), sourceChemin.size()+1);
        argv[iteratorParams] = (const char*) libP2;
        
        iteratorParams++;
    }

#ifdef _WIN32
    //LLVM_MATH is added to resolve mathematical float functions, like powf on windows
    argv[iteratorParams] = "-l";
    iteratorParams++;
    argv[iteratorParams] = "llvm_math.ll";
    iteratorParams++;
#endif
    
    //Parsing the compilationOptions from a string to a char**
    QString copy = faustOptions;
    
    for (int i = numberFixedParams; i<  argc; i++) {
        
        string parseResult(parse_compilationParams(copy));
        char* intermediate = new char[parseResult.size()+1];
        strcpy(intermediate,parseResult.c_str());
        
        //        OPTION DOUBLE HAS TO BE SKIPED, it causes segmentation fault
        if (strcmp(intermediate, "-double") != 0) {
            argv[i] = (const char*)intermediate;
        } else{
            argc--;
            i--;
        }
    }

    return argv;
}
//--Remote params
const char** FLSessionManager::getRemoteInstanceArgv(QSettings* winSettings, int& argc)
{
    argc = 12;
    const char** argv = new const char*[argc];
    
    argv[0] = "--NJ_ip";
    QString localString = searchLocalIP();
    string ip(localString.toStdString());
    
    char* ipString = new char[ip.size()+1];
    strncpy(ipString, ip.c_str(), ip.size()+1);
    argv[1] = (const char*)ipString;
    
    argv[2] = "--NJ_latency";
    QString latency = winSettings->value("RemoteProcessing/Latency", "10").toString();
    string lat = latency.toStdString();
    
    char* latString = new char[lat.size()+1];
    strncpy(latString, lat.c_str(), lat.size()+1);
    argv[3] = (const char*)latString;
  
    argv[4] = "--NJ_compression";
    QString cv = winSettings->value("RemoteProcessing/CV", "64").toString();
    string compression = cv.toStdString();
    char* cvString = new char[compression.size()+1];
    strncpy(cvString, compression.c_str(), compression.size()+1);
    argv[5] = (const char*)cvString;
    
    argv[6] = "--NJ_mtu";
    QString mtu = winSettings->value("RemoteProcessing/MTU", "1500").toString();
    string mtuVal = mtu.toStdString();
    char* mtuString = new char[mtuVal.size()+1];
    strncpy(mtuString, mtuVal.c_str(), mtuVal.size()+1);
    argv[7] = (const char*)mtuString;
    
    argv[8] = "--NJ_buffer_size";
    QString buffer_size = winSettings->value("BufferSize", 512).toString();
    string buffer_sizeVal = buffer_size.toStdString();
    char* buffer_sizeString = new char[buffer_sizeVal.size()+1];
    strncpy(buffer_sizeString, buffer_sizeVal.c_str(), buffer_sizeVal.size()+1);
    argv[9] = (const char*)buffer_sizeString;
    
    argv[10] = "--NJ_sample_rate";
    QString sample_rate = winSettings->value("SampleRate", 44100).toString();
    string sample_rateVal = sample_rate.toStdString();
    char* sample_rateString = new char[sample_rateVal.size()+1];
    strncpy(sample_rateString, sample_rateVal.c_str(), sample_rateVal.size()+1);
    argv[11] = (const char*)sample_rateString;
    
    return argv;
}

//--Delete params
void FLSessionManager::deleteArgv(int argc, const char** argv)
{
    for (int i = 0; i < argc; i++)
        delete argv[i];
    delete[] argv;
}

//------------------- Generation of auxilary files ----------------------
QString FLSessionManager::getErrorFromCode(int code)
{
#ifdef REMOTE
    if(code == ERROR_FACTORY_NOTFOUND) {
        return "Impossible to create remote factory";
    } else if (code == ERROR_INSTANCE_NOTCREATED) {
        return "Impossible to create DSP Instance";
    } else if(code == ERROR_NETJACK_NOTSTARTED) {
        return "NetJack Master not started";
    } else if (code == ERROR_CURL_CONNECTION) {
        return "Curl connection failed";
    }
#else
	Q_UNUSED(code);
#endif
    return "ERROR not recognized";
}

bool FLSessionManager::generateAuxFiles(const QString& shaKey, const QString& sourcePath, const QString& faustOptions, const QString& name, QString& errorMsg)
{
    updateFolderDate(shaKey);
    int argc;
    const char** argv = getFactoryArgv(sourcePath, faustOptions, argc);
    QString sourceFile = fSessionFolder + "/SHAFolder/" + shaKey + "/" + shaKey + ".dsp";

	if (faustOptions != "") {
        std::string error;
        if(!generateAuxFilesFromString(name.toStdString(), pathToContent(sourceFile).toStdString(), argc, argv, error)){
            errorMsg = error.c_str();
            return false;
        }
    }
    
    return true;
}

bool FLSessionManager::generateSVG(const QString& shaKey, const QString& sourcePath, const QString& svgPath, const QString& name, QString& errorMsg)
{
    updateFolderDate(shaKey);
    int argc = 7;
    if (sourcePath == "")
        argc = argc-2;
    
    int iteratorParams = 0;
    
#ifdef _WIN32
    argc = argc+2;
#endif
    
    const char** argv = new const char*[argc];
    argv[iteratorParams] = "-I";
    iteratorParams++;
    
    //The library path is where libraries like the scheduler architecture file are = currentSession
    string libsFolder = fSessionFolder.toStdString() + "/Libs";
    string libPath = libsFolder;
    char* libP = new char[libsFolder.size()+1];
    strncpy( libP, libPath.c_str(), libsFolder.size()+1);
    argv[iteratorParams] = (const char*) libP;
    iteratorParams++;
    
    if (sourcePath != "") {
        argv[iteratorParams] = "-I";   
        iteratorParams++;
        
        QString sourceChemin = QFileInfo(sourcePath).absolutePath();
        string path = sourceChemin.toStdString();
        
        char* libP2 = new char[sourceChemin.size()+1];
        strncpy( libP2, path.c_str(), sourceChemin.size()+1);
        argv[iteratorParams] = (const char*) libP2;
        
        iteratorParams++;
    }

#ifdef _WIN32
    //LLVM_MATH is added to resolve mathematical float functions, like powf
    argv[iteratorParams] = "-l";
    iteratorParams++;
    argv[iteratorParams] = "llvm_math.ll";
    iteratorParams++;
#endif
    
    string pathSVG = svgPath.toStdString();
    char* svgP = new char[pathSVG.size()+1];
    strncpy(svgP, pathSVG.c_str(), pathSVG.size()+1);
    
    argv[iteratorParams] = "-svg";
    iteratorParams++;
    argv[iteratorParams] = "-O";
    iteratorParams++;
    argv[iteratorParams] = (const char*) svgP;
    
    QString sourceFile = fSessionFolder + "/SHAFolder/" + shaKey + "/" + shaKey + ".dsp";
    
	std::string error;
    if (!generateAuxFilesFromString(name.toStdString(), pathToContent(sourceFile).toStdString(), argc, argv, error)) {
        errorMsg = error.c_str();
        return false;
    }
    
    return true;
}

//Calculate the faust expanded version
QString FLSessionManager::getExpandedVersion(QSettings* settings, const QString& source)
{
    string name_app = settings->value("Name", "").toString().toStdString();
    string sha_key = settings->value("SHA", "").toString().toStdString();
    string dsp_content = source.toStdString();
    
    if (QFileInfo(source).exists()) {
        dsp_content = pathToContent(source).toStdString();
    }
    
    int argc = 0;
    QString defaultOptions = FLSettings::_Instance()->value("General/Compilation/FaustOptions", "").toString();
    QString faustOptions = settings->value("Compilation/FaustOptions", defaultOptions).toString();
    const char** argv = getFactoryArgv(settings->value("Path", "").toString(), faustOptions, argc);
    string error_msg("");
    
    return QString(expandDSPFromString(name_app, dsp_content, argc, argv, sha_key, error_msg).c_str());
}

//-----------------------Session Management----------------------------------

//---Managing SHAFolder content. SHAFolder modified date is updated whenever a file of the folder is used. Then when there are too many folder saved, the most former used folder is deleted
void FLSessionManager::updateFolderDate(const QString& shaValue)
{
    QString shaFolder = fSessionFolder + "/SHAFolder/" + shaValue;
    touchFolder(shaFolder);
}

void FLSessionManager::cleanSHAFolder()
{
    QString shaFolder = fSessionFolder + "/SHAFolder";
    QDir shaDir(shaFolder);
    QFileInfoList children = shaDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    
    if (children.size() > kMaxSHAFolders) {
        for (QFileInfoList::iterator it = children.begin(); it != children.end(); it++) {
            if (it == (children.end()-1)){
                QString childToDelete = it->absoluteFilePath();
                deleteDirectoryAndContent(childToDelete);
            }
        }
    }
}

//Saving the sources of the windows in their designated folders
void FLSessionManager::saveCurrentSources(const QString& sessionFolder)
{
    FLSettings* generalSettings = FLSettings::_Instance();
    generalSettings->beginGroup("Windows");
    QStringList groups = generalSettings->childGroups();
    
    for (int i = 0; i<groups.size(); i++) {
                
        QString settingsPath = groups[i] + "/SHA";
        QString shaValue = generalSettings->value(settingsPath, "").toString();
        
        QString shaPath = fSessionFolder + "/SHAFolder/" + shaValue + "/" + shaValue + ".dsp";
        QString savedPath = sessionFolder + "/Windows/FLW-" + groups[i] + "/FLW-" + groups[i] + ".dsp";
        
        QFile toRemove(savedPath);
        toRemove.remove();
        
        QFile shaSource(shaPath);
        shaSource.copy(savedPath);
    }
    generalSettings->endGroup();
}

//--Ask the user if he wants to save its DSP in an new location (when the source is not a file)
QString FLSessionManager::askForSourceSaving(const QString& sourceContent)
{
    //------SLOTS FROM MENU ACTIONS THAT ARE REDIRECTED
    QMessageBox* existingNameMessage = new QMessageBox(QMessageBox::Warning, tr("Notification"), "This DSP is not attached to any file.\n Do you want to save your code in a new file?");
    
    QPushButton* yes_Button = existingNameMessage->addButton(tr("Yes"), QMessageBox::AcceptRole);
    existingNameMessage->addButton(tr("Cancel"), QMessageBox::RejectRole);
    existingNameMessage->exec();
    
    if (existingNameMessage->clickedButton() == yes_Button) {
        
        QFileDialog* fileDialog = new QFileDialog;
        fileDialog->setConfirmOverwrite(true);
        QString filename = fileDialog->getSaveFileName(NULL, "Save DSP", tr(""), tr("(*.dsp)"));
        
        if (QFileInfo(filename).suffix().indexOf("dsp") == -1)
            filename += ".dsp";
        
        writeFile(filename, sourceContent);
        return filename;
    } else {
        return "";
    }
}

//--Returns the content of sha file contained in the SHAFolder
QString FLSessionManager::contentOfShaSource(const QString& shaSource)
{
    QString shaPath = fSessionFolder + "/SHAFolder/" + shaSource + "/" + shaSource + ".dsp";
    return pathToContent(shaPath);
}

//--Restoration Menu when a problem is emerging at session recalling time
bool FLSessionManager::viewRestorationMsg(const QString& msg, const QString& yesMsg, const QString& noMsg)
{
    QMessageBox* existingNameMessage = new QMessageBox(QMessageBox::Warning, tr("Notification"), msg);
    QPushButton* yes_Button;
    
    existingNameMessage->setText(msg);
    yes_Button = existingNameMessage->addButton(yesMsg, QMessageBox::AcceptRole);
    existingNameMessage->addButton(noMsg, QMessageBox::RejectRole);
    existingNameMessage->exec();
    
    return (existingNameMessage->clickedButton() == yes_Button);
}

//Behaviour of session restoration when re-starting the application
//The user is notified in case of source file lost or modified. He can choose to reload from original file or backup.
map<int, QString> FLSessionManager::currentSessionRestoration()
{
    map<int, QString> windowIndexToSource;
//If 2 windows are pointing on the same lost source, the Dialog has not to appear twice
    QMap<QString, int> updated;
    FLSettings* generalSettings = FLSettings::_Instance();
    generalSettings->beginGroup("Windows");
    
    QStringList groups = generalSettings->childGroups();
    for (int i = 0; i < groups.size(); i++) {
               
        QString shaPath = groups[i] + "/SHA";
        QString shaValue = generalSettings->value(shaPath, "").toString();
        
        QString recallingPath = fSessionFolder + "/Windows/FLW-" + groups[i] + "/FLW-" + groups[i] + ".dsp";
        QString savedContent = pathToContent(recallingPath);
        
        QString settingsPath = groups[i] + "/Path";
        QString originalPath = generalSettings->value(settingsPath, "").toString();
        
//        In Case DSP Source Is Not a DSP File
        if (originalPath == "") {
            windowIndexToSource[groups[i].toInt()] = savedContent;
//        In case it is a DSP
        } else{
//              In case path restoration was already treated
            if(updated.contains(originalPath)){
                windowIndexToSource[groups[i].toInt()] = windowIndexToSource[updated[originalPath]];
            } else {

                QString originalContent = pathToContent(originalPath);
                
                //            In Case The Original File Was Deleted
                if (!QFileInfo(originalPath).exists()) {
                    QString msg = originalPath + " cannot be found! Do you want to reload it from an internal copy of your file?";
                    if (viewRestorationMsg(msg, "Yes", "No")) {
                        windowIndexToSource[groups[i].toInt()] = savedContent;
                    } else {
                        windowIndexToSource[groups[i].toInt()] = "";
                    }
                }
                //            In Case The Original Content is Modified
                else if(QFileInfo(recallingPath).exists() && savedContent != originalContent) {
                    
                    QString msg = "The content of " + originalPath + " was modified. Do you want to reload " + originalPath+ " or an unmodified internal copy?";
                    
                    if (viewRestorationMsg(msg, "Internal Copy", "Path")) {
                        windowIndexToSource[groups[i].toInt()] = savedContent; 
                    } else {
                        windowIndexToSource[groups[i].toInt()] = originalPath;
                    }
                }
                //             In Normal Case
                else {
                    windowIndexToSource[groups[i].toInt()] = originalPath;
                }
                
//                Declaration as treated
                updated[originalPath] = groups[i].toInt();
            }
        }
            
    }
    
    generalSettings->endGroup();
    return windowIndexToSource;
}

void FLSessionManager::copySHAFolder(const QString& snapshotFolder)
{
    QString shaFolder = fSessionFolder + "/SHAFolder";
    QString shaSnapshotFolder = snapshotFolder + "/SHAFolder";
    
    QDir snapshotDir(shaSnapshotFolder);
    QFileInfoList children = snapshotDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    QFileInfoList::iterator it;
    
    for (it = children.begin(); it != children.end(); it++) {
        QString destinationFolder = shaFolder + "/" +  it->baseName();
        cpDir(it->absoluteFilePath(), destinationFolder);
    }
}

map<int, QString>   FLSessionManager::snapshotRestoration(const QString& file)
{
    QString filename = file;
    
#ifndef _WIN32    
    if (QFileInfo(filename).completeSuffix() != "tar")
        filename += ".tar";

    QString error;
    if (!untarFolder(filename, error))
        FLErrorWindow::_Instance()->print_Error(error);
#endif
    
    QString snapshotFolder = QFileInfo(filename).canonicalPath() + "/" + QFileInfo(filename).baseName();
    copySHAFolder(snapshotFolder);
    map<int, QString> windowIndexToSource;
    
    //If 2 windows are pointing on the same lost source, the Dialog has not to appear twice
    QMap<QString, int> updated;
    QString snapshotSettings = snapshotFolder + "/Settings.ini";
    QSettings* generalSettings = new QSettings(snapshotSettings, QSettings::IniFormat);
    generalSettings->beginGroup("Windows");
    QStringList groups = generalSettings->childGroups();
    
    for (int i = 0; i < groups.size(); i++) {
        
        QString shaPath = groups[i] + "/SHA";
        QString shaValue = generalSettings->value(shaPath, "").toString();
        QString recallingPath = snapshotFolder + "/Windows/FLW-" + groups[i] + "/FLW-" + groups[i] + ".dsp";
        QString savedContent = pathToContent(recallingPath);
        QString settingsPath = groups[i] + "/Path";
        QString originalPath = generalSettings->value(settingsPath, "").toString();
        
        //        In Case DSP Source Is Not a DSP File
        if (originalPath == "") {
            windowIndexToSource[groups[i].toInt()] = savedContent;
        //        In case it is a DSP
        } else {
            //              In case path restoration was already treated
            if (updated.contains(originalPath)) {
                windowIndexToSource[groups[i].toInt()] = windowIndexToSource[updated[originalPath]];
            } else {
        
                QString originalContent = pathToContent(originalPath);
    
                //            In Case The Original File Was Deleted
                if (!QFileInfo(originalPath).exists() || savedContent != originalContent){
                    windowIndexToSource[groups[i].toInt()] = savedContent;
                    
                    QString errorMsg;
                    if (!QFileInfo(originalPath).exists())
                        errorMsg = originalPath + " was deleted. An internal copy is used to recall your DSP.";
                    else
                        errorMsg = "The content of " + originalPath + " was modified. An internal copy is used to recall your DSP";
                    
                    FLErrorWindow::_Instance()->print_Error(errorMsg);
                    // ET IL FAUT FAIRE UN TRUC POUR PAS QU'ON TE LE RAPPELLE À CHAQUE RAPPEL DE CETTE SESSION 
                } else {
                    windowIndexToSource[groups[i].toInt()] = originalPath;
                }
                updated[originalPath] = groups[i].toInt();
            }
        }
    }
    
    generalSettings->endGroup();
    return windowIndexToSource;
}

// To know if the contant was modified or the path, the original file has to be compared to the SHA saved file
void FLSessionManager::createSnapshot(const QString& snapshotFolder)
{
    QDir snapshot(snapshotFolder);
    snapshot.mkdir(snapshotFolder);
    
    QString shaFolder = fSessionFolder + "/SHAFolder";
    QString shaSnapshotFolder = snapshotFolder + "/SHAFolder";
    
    QDir shaDir(shaSnapshotFolder);
    shaDir.mkdir(shaSnapshotFolder);
    
    QSettings* generalSettings = FLSettings::_Instance();
    
    generalSettings->beginGroup("Windows");
    
    QStringList groups = generalSettings->childGroups();
    
    for (int i = 0; i < groups.size(); i++) {
        
        QString shaCS = groups[i] + "/SHA";
        QString shaSF = generalSettings->value(shaCS, "").toString();

        QString srcFolder = shaFolder + "/" + shaSF;
        QString dstFolder = shaSnapshotFolder + "/" + shaSF;
        
        QDir dstDir(dstFolder);
        dstDir.mkdir(dstFolder);
        
        cpDir(srcFolder, dstFolder);
    }
    
    generalSettings->endGroup();
    
    QString winFolder = fSessionFolder + "/Windows";
    QString winSnapshotFolder = snapshotFolder + "/Windows";
    
    cpDir(winFolder, winSnapshotFolder);
    
    QString settingsFile = fSessionFolder + "/Settings.ini";
    QString settingsSnapshotFile = snapshotFolder + "/Settings.ini";
    
    QFile f(settingsFile);
    f.copy(settingsSnapshotFile);
    
    saveCurrentSources(snapshotFolder);

#ifndef _WIN32
    QString error("");
    if(tarFolder(snapshotFolder, error))
        deleteDirectoryAndContent(snapshotFolder);
    else
        FLErrorWindow::_Instance()->print_Error(error);
#endif
}

//----------------------- Handle faust file dependencies ------------------

QVector<QString> FLSessionManager::getDependencies(llvm_dsp_factory* factoryDependency)
{
    QVector<QString> dependencies;
    std::vector<std::string> stdDependendies;
    stdDependendies = getLibraryList(factoryDependency);

    for (size_t i = 0; i<stdDependendies.size(); i++) {
        dependencies.push_back(stdDependendies[i].c_str());
    }

    return dependencies;
}

QVector<QString> FLSessionManager::readDependencies(const QString& shaValue)
{
    QVector<QString> dependencies;
    QString shaPath =  fSessionFolder + "/SHAFolder/" + shaValue + "/" + shaValue + ".ini";
    QSettings* settings = new QSettings(shaPath, QSettings::IniFormat);
    QStringList groups  = settings->childKeys();
    
    for (int i = 0; i < groups.size(); i++) {
        QString dependency = settings->value(QString::number(i), "").toString();
        dependencies.push_back(dependency);
    }
    
    return dependencies;
}

void FLSessionManager::writeDependencies(QVector<QString> dependencies, const QString& shaValue)
{
    QString shaPath =  fSessionFolder + "/SHAFolder/" + shaValue + "/" + shaValue + ".ini";
    QSettings* settings = new QSettings(shaPath, QSettings::IniFormat);
    
    for (int i = 0; i < dependencies.size(); i++) {
        settings->setValue(QString::number(i), dependencies[i]);
    }
}

//---------------------PUBLISH FACTORIES ON LOCAL SERVER------------------
/* This is an attempt not finished to implement a publish service of DSP*/
//#ifdef REMOTE
//Add Window to Server through createRemoteFactory...
//bool FLSessionManager::addWinToServer(FLWinSettings* settings){
//    
//    QString sha_key = settings->value("SHA", "").toString();
//    string name = settings->value("Name", "").toString().toStdString()/*+ "_" + QString::number(settings->value("Release/Number", 0).toInt()).toStdString()*/;
//    
//    QString factoryFolder = fSessionFolder + "/SHAFolder/" + sha_key;
//    
//    QString fileToCompile = factoryFolder + "/" + sha_key + ".dsp";
//    
//    string error;
//    
//    QString faustOptions = settings->value("Compilation/FaustOptions", "").toString();
//    int optLevel = settings->value("Compilation/OptValue", 3).toInt();
//    
//    int argc;
//	const char** argv = getFactoryArgv(settings->value("Path", "").toString(), faustOptions, argc);
//    
//    int portValue = FLSettings::_Instance()->value("General/Network/RemoteServerPort", 5555).toInt();
//    
//    remote_dsp_factory* factory = createRemoteDSPFactoryFromString(name, pathToContent(fileToCompile).toStdString(), argc, argv, searchLocalIP().toStdString(), portValue, error, optLevel);
//    
//    if(factory){
//        fPublishedFactories[sha_key] = factory;
//        settings->setValue("Release/Number", settings->value("Release/Number", 0).toInt()+1);
//        return true;
//    }
//    else
//        return false;
//}

//Delete Window From Server through createRemoteFactory...
//void FLSessionManager::deleteWinFromServer(FLWinSettings* settings){
//    
//    QString sha_key = settings->value("SHA", "").toString();
//    
//    remote_dsp_factory* factory = fPublishedFactories[sha_key];
//    if(factory)
//        deleteRemoteDSPFactory(factory);
//    
//}
//#endif






