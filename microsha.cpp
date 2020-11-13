#include <iostream>
#include <unistd.h>
#include <string>
#include <cstring>
#include <sys/wait.h>
#include <fstream>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <cstdlib>
#include <vector>
#include <dirent.h>
#include <algorithm>

using namespace std;

volatile sig_atomic_t pid_run; // pid процесса, обрабатывающего команду

bool flag_command_read; //  true если сейчас ждем ввода

void my_stop (int sig) { //  обработка сигала
    if ( flag_command_read )
        return;
    if ( pid_run == 0 )
        exit(0);
    else
        kill(pid_run, SIGKILL);
}

struct command
{
    vector<char*> argv; // массив аргументов
    char* file_input; // перенапревление ввода
    char* file_output; // перенапревление вывода

    int pid; // pid дочернего процесса

    command* follow_pipe; // если есть  конвейер

    command() {
        file_input = file_output = nullptr;
        follow_pipe = nullptr;
    }

    ~command () {
    if ( this->follow_pipe != nullptr )
        delete this->follow_pipe;
    if ( this->file_input )
        free(this->file_input );
    if ( this->file_output )
        free(this->file_output);
    for ( auto it : this->argv )
        if ( it )
            free(it);
    }
};


bool change_in ( char* name_file) { // перенапрпаление ввода
    int fd = open (name_file, O_RDONLY );
    int temp = dup2(fd , STDIN_FILENO);
    if ( temp == -1 ) {
        cerr<<"ошибка открытия файла: "<<name_file<<'\n';
        return false;
    }
    close(fd);
    return true;
}

bool change_out ( char* name_file) { // перенапрпаление ввода
    int fd = open (name_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int temp = dup2(fd , STDOUT_FILENO);
    if ( temp == -1 ) {
        cerr<<"ошибка открытия файла: "<<name_file<<'\n';
        return false;
    }
    close(fd);
    return true;
}

void my_cd (vector<char*> ARG) { // смена дирректории
    if (ARG[1] == nullptr ) {
        string temp = getenv("USER");
        temp = "/home/" + temp;
        chdir( temp.c_str());
        return;
    }
    if ( chdir(ARG[1]) != 0 )
        cerr<<"ошибка cd: каталог недоступен"<<'\n';
    else
        return;
}


void pipe_run (command* ptr) {  // обработка конвейера
    pid_t wpid;
    int pfd[2], input, status;
    bool first = true; // первая ли команда в конвейре
    for (command *cmd=ptr; cmd; cmd = cmd->follow_pipe) { // поочередный запуск процессов конвейера
        pipe(pfd);
        if (!(cmd->pid = fork())) {
            if (!first) {
                dup2(input, 0);
                close(input);
            }
            if ( cmd->follow_pipe != nullptr )
                dup2(pfd[1], 1);
            close(pfd[0]);
            close(pfd[1]);
            bool flag_open_in = true, flag_open_out = true; // удалось ли открыть файл для перенаправления
            if ( cmd->file_input ) {
                if ( !first) {
                    cerr<<"ошибка: не первая команда конвейера имеет перенаправление ввода"<<'\n';
                    return;
                }
                flag_open_in = change_in (cmd->file_input);
            }
            if ( cmd->file_output ){
                if ( cmd->follow_pipe != nullptr ) { 
                    cerr<<"ошибка: не последняя команда конвейера имеет перенаправление выводы"<<'\n';
                    return;
                }
                flag_open_out = change_out (cmd->file_output);
            }
            if ( flag_open_in && flag_open_out ) {
                execvp( cmd->argv[0], &cmd->argv[0]);
                exit(0);
            }
            else {
                return;
            }
        }
        if (!first)
            close(input);
        close(pfd[1]);
        input = pfd[0];
        first = false;
    }
    for (command *cmd=ptr; cmd; cmd = cmd->follow_pipe) {  // ожидание завершения порожденных процессов
        do {
            wpid = waitpid(cmd->pid, &status, WUNTRACED);
        } while (!WIFEXITED(status) && !WIFSIGNALED(status));
    }
}



 
void command_run ( command*  pCmd) { // запуск команды
    pid_t wpid ;
    int status;
    string cd = "cd"; 
    string set = "set";
    if (pCmd->argv[0] == cd ) {
        my_cd(pCmd->argv);
        return;
    }
    if (pCmd->argv[0] == set) {
        setenv(pCmd->argv[1], pCmd->argv[2], 1);
        return ;
    }
    pid_run = fork();
    if ( pid_run == 0 ) {
        if ( pCmd->follow_pipe != nullptr ) {
            pipe_run (pCmd );
            exit(0);
        }
        else {
            bool flag_open_in = true, flag_open_out = true;
            if ( pCmd->file_input ) {
                flag_open_in = change_in (pCmd->file_input);
            }
            if ( pCmd->file_output ){
                flag_open_out = change_out (pCmd->file_output);
            }
            if ( flag_open_in && flag_open_out ) {
                execvp( pCmd->argv[0], &pCmd->argv[0]);
                exit(0);
            }
            else {
                return;
            }
        }
    }
    else {
        do {
                wpid = waitpid(pid_run, &status, WUNTRACED);
            } while (!WIFEXITED(status) && !WIFSIGNALED(status));
    }
}

bool match ( unsigned int i, unsigned int j, string & s1, string &s2) { // проверка подходит ли s1 под маску s2
    if ( ( i >= s1.size()  ) && j >= s2.size() ) // обе строки прочитаны
        return true;
    if ( j == s2.size() && i == s1.size()-1 && s1[i] =='*')
        return true;
    if ( (i < s1.size() && j >= s2.size() ) || (i >= s1.size() && j < s2.size() )  ) { // прочитана только одна строка
        return false;
    }
    switch ( s1[i] ) {
    case '?' : { // пропустить ровно 1 символ
        return match (i+1, j+1, s1, s2);
        break;
    }
    case '*' : {  //пропустить сколько угодно символов
        bool ans = false;
        for (int x = 0; j+x <= s2.size(); ++x)
            ans = (ans || match(i+1, j+x, s1, s2) );
        return ans;
        break;
    }
    default : { // обе буквы
        if ( s1[i] == s2[j] )
                return match(i+1, j+1, s1, s2) ;
       else
            return false;
    }
    }
}

bool not_only_slashes_dots ( string  s) { // для удаления "побочных" по типу ./. пояляющихся при обходе дирректории 
  unsigned int idx = 0;
  while ( idx < s.size() ) {
    switch ( s[idx] ) {
      case '.': ++idx ; break;
      case '/': ++idx; break;
      default : return true;
    }
  }
  return false;
}


void ls(string const &name, bool recursive, vector<string> &ret) { // обход дирректории
  DIR *dir = opendir(name.c_str());
  if (dir == nullptr) return;
  for (dirent *d = readdir(dir); d != nullptr; d = readdir(dir)) {
    if (recursive && d->d_type == DT_DIR) {
      if (strcmp(d->d_name, ".") == 0) continue;
      if (strcmp(d->d_name,"..") == 0) continue;
      ls(name + string("/") + d->d_name, true, ret);
    } else {
        if ( not_only_slashes_dots( (string)d->d_name) )
          ret.push_back(  d->d_name);
    }
  }
  closedir(dir);
}


pair<string, int> extract_word (int idx, string & line) { // извлечь непрерывного слова, окруженное пробелами
    int ptr = idx;
    string ans;
    while ( line[ptr] == ' ' || line[ptr] == '\t')
        ++ptr;
    while ( !(line[ptr] == ' ' || line[ptr] == '\t' || line[ptr] == '\n' || line[ptr] == '\000' || line[ptr] == '>' || line[ptr] == '<' || line[ptr] == '|') ) {
        ans.push_back(line[ptr]);
        ++ptr;
    }
    return make_pair(ans, ptr  - idx);
}

pair<int, int> find_first_leksem (string & s) { // найти первое вхождение элементарной лексемы, содержащей не более одного метасимвола
    pair<int, int> ans (-2, -1); // левая и правая границы fistr == -2 значит лексемы нет
    for (unsigned int i = 0; i < s.size(); ++i )
        if ( s[i] == '?' || s[i] == '*') { // если метасимвол то есть вхождение лексемы
            ans.first = -1;
            for ( int j = i ; j >= 0; --j )
                if ( s[j] == '/' ) {
                    ans.first = j;  // отделить путь  "до" от самой лекссемы
                    break;
                }
            for ( unsigned int j = i ; j < s.size() ; ++j)
                if ( s[j] == '/' ) {
                    ans.second = j; // отделить путь  "после" от самой лекссемы 
                    break;
                }
            break;
        }
    if (ans.second == -1)
        ans.second = s.size();
    return ans;
}


void replace_leksem (vector<string> & ARG) { // заменить все лексемы
    int i = 0;
    while (i < ARG.size() ) {
        pair< int, int> bordes_first_leksem = find_first_leksem(ARG[i]);
        if ( bordes_first_leksem.first == -2) { // не содержит лексемы
            ++i;
            continue;
        }
        string leksema, way, tail = ""; // путь  -- сама лексема -- "хвост"
        if (bordes_first_leksem.first == -1) {
            way = "."; // путь по умолчанию
        }
        for ( int j = 0; j < ARG[i].size(); ++j ) { // парсинг на три части
            if ( j <= bordes_first_leksem.first )
                way.push_back(ARG[i][j]);
            else {
                if ( j < bordes_first_leksem.second)
                    leksema.push_back(ARG[i][j]);
                else
                    tail.push_back(ARG[i][j]);
            }
        }
        while ( leksema.size() > 1) { // удалить двойные * в конце
            if ( leksema[ (int)leksema.size()-1 ] == '*' && leksema[leksema.size()-2] == '*' )
                leksema.pop_back();
            else
                break;
        }
        vector<string> probably_name ; // все файлы по указанному пути
        ls( way,false ,probably_name);
        vector<string> good_name; // файлы удовлетворяющие лексеме
        for (auto it : probably_name )
            if ( match(0, 0, leksema, it) )
                good_name.push_back(it);
        if ( i == ARG.size() - 1) { // удаляем обработанную лексему, заменяем ее на все подходящие файлы
            ARG.pop_back();
        }
        else {
            swap (ARG[i], ARG[ARG.size()-1]);
            ARG.pop_back();
        }
        for ( auto it : good_name ) {
            string temp;
            if ( way == ".")
                temp = it+tail;
            else
                temp = way + it + tail;
            ARG.push_back(temp);
        }
    }
}

bool extract_command ( command* pCmd , unsigned int  idx, string & line) { // парсер исходной стоки на команды
    vector<string> ARG; // аргументы
    string file_in, file_out;
    pair<string, int> word;  // выделеннное слово между пробелов
    bool flag = true, temp_flag;
    while ( idx < line.size() ) {
        switch ( line[idx]  ) {
        case ' ' :
            ++idx;
            break;
        case '\t' :
            ++idx;
            break;
        case '|' :
            pCmd->follow_pipe = new command ;
            temp_flag = extract_command(pCmd->follow_pipe,  idx+1, line);
            if ( flag )
                flag = temp_flag;
            idx = line.size()+1 ;
            break;
        case '>' :
            if ( !file_out.empty()) {
                cerr<<"ошибка: двойное перенаправление вывода в файл"<<'\n';
                return false;
            }
            word = extract_word(idx+1, line);
            file_out = word.first;
            idx += word.second;
            ++idx;
            break;
        case '<' :
            if ( !file_in.empty()) {
                cerr<<"ошибка: двойное перенаправление ввода из файла"<<'\n';
                return false;
            }
            word = extract_word(idx+1, line);
            file_in = word.first;
            idx += word.second;
            ++idx;
            break;
        case '-' :
            word = extract_word(idx, line);
            ARG.push_back(word.first);
            idx +=word.second;
            break;
        default :
            word = extract_word(idx, line);
            idx += word.second;
            ARG.push_back(word.first);
            if ( ARG.size() == 1 && ARG[0] == "time" )
                ARG.push_back("-p");
        }
    }
    int size_ARG = ARG.size(); // проверка что есть подходящие под лексему слова, после замены размер вектора не должен уменьшится
    replace_leksem(ARG);
    if (ARG.size() < size_ARG ){
        cerr<<"ошибка: не найдено ни одного объекта, удовлетоворяющего маске"<<'\n';
        return false;
    }
    for (size_t i = 0; i < ARG.size(); i++)
       pCmd->argv.push_back((char *)strdup( ARG[i].c_str() ) );
    pCmd->argv.push_back(nullptr);
    if ( ! file_in.empty())
        pCmd->file_input =(char*) strdup( file_in.c_str() );
    if ( ! file_out.empty())
        pCmd->file_output = (char*)strdup( file_out.c_str() );
    return true;
}


void print_head_line () { // печатать приглашение
    char cwd[2048];
    getcwd(cwd, sizeof(cwd));
    if ( strcmp( "root", (const char*) getenv("USER") ) )
        cout<<cwd<<'>';
    else
        cout<<cwd<<'!';
}


int main () {
    signal(SIGINT, my_stop); // перехват сигнала
    string s ;
    print_head_line();
    flag_command_read = true;
    while ( getline(cin, s)) {
        if ( !s.empty() ) {
            command* temp = new command;
            flag_command_read = false;
            bool flag = extract_command (temp, 0,  s);
            if ( flag){
                command_run ( temp );
            }
            delete temp;
        }
        print_head_line();
        flag_command_read = true;
    }
    return 0;
}