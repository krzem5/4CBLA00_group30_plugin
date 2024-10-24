/*
 * Copyright (c) Krzesimir Hyżyk - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Created on 08/10/2024 by Krzesimir Hyżyk
 */



#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <unistd.h>



#define ASSERT(x) if (!(x)){printf("%u(%s): %s: Assertion failed\n",__LINE__,__func__,#x);_Exit(1);}
#define CHECK_STR(x,str) (!memcmp((x),str,sizeof(str)))
#define CHECK_STR_PREFIX(x,str) (!memcmp((x),str,sizeof(str)-1))

#define PI_SQUARED 9.869604401089358

#define PARSER_STATE_NONE 0
#define PARSER_STATE_COORDINATES 1
#define PARSER_STATE_CONNECTIVITY 2
#define PARSER_STATE_GEOMETRY 3
#define PARSER_STATE_GEOMETRY_CONTENT 4
#define PARSER_STATE_POINT_LOAD 5
#define PARSER_STATE_NODE_RESULTS 6
#define PARSER_STATE_DISPLACEMENTS 7



typedef union _POINT{
	struct{
		double x;
		double y;
		double z;
		double dx;
		double dy;
		double dz;
	};
	double coords[6];
} point_t;



typedef struct _CONNECTION{
	uint32_t a;
	uint32_t b;
} connection_t;



typedef struct _MATERIAL_TYPE{
	double bias;
	connection_t* connections;
	uint32_t connection_count;
} material_type_t;



static const char _ssh_script[]="@echo off\nsetlocal\ncd /d \"%%~dp0\"\nfor /l %%%%i in (1,1,%%1) do (\n\tstart \"\" cmd /c 9>\"%%temp%%\\wait%%%%i.lock\" <THIS PART OF THE SCRIPT HAS BEEN REDACTED FOR SECURITY REASONS>%s\n) 2>nul\n:wait\n1>nul 2>nul ping /n 2 ::1\nfor /l %%%%i in (1,1,%%1) do (\n\t(call ) 9>\"%%temp%%\\wait%%%%i.lock\" || goto :wait\n\tdel /f /q \"%%temp%%\\wait%%%%i.lock\"\n) 2>nul\n";
static const char _ssh_command[]="sshpass -p %s ssh %s@%s \"%s\\exec.bat %u\"";



static point_t* _points=NULL;
static uint32_t _point_count=0;
static material_type_t* _material_types=NULL;
static uint32_t _material_type_count=0;
static int _src_file_fd=-1;
static uint32_t _point_load_line_start=0;
static uint32_t _point_load_line_end=0;
static uint32_t _job_file_length=0;
static char _output_file_path[256];
static uint32_t _output_file_path_prefix_length=0;
static const char* _vm_output_dir=NULL;
static double _bounds[2];
static uint32_t _divisions=0;



static void _ensure_material_type_key(uint32_t idx){
	if (_material_type_count>idx){
		return;
	}
	_material_types=realloc(_material_types,(idx+1)*sizeof(material_type_t));
	for (;_material_type_count<=idx;_material_type_count++){
		(_material_types+_material_type_count)->connections=NULL;
		(_material_types+_material_type_count)->connection_count=0;
	}
}



static void _load_job_data(const char* path){
	_src_file_fd=open(path,O_RDONLY);
	ASSERT(_src_file_fd>=0);
	char line[4096];
	FILE* file=fopen(path,"rb");
	uint32_t state=PARSER_STATE_NONE;
	uint32_t line_index=0;
	uint32_t current_geometry_key=0;
	while (1){
		char c;
		do{
			c=fgetc(file);
			if (c==EOF){
				goto _eof;
			}
		} while (c=='\n');
		uint32_t start_line_index=ftell(file)-1;
		uint32_t line_length=0;
		for (;c!=EOF&&c!='\n';line_length++){
			line[line_length]=c;
			c=fgetc(file);
		}
		for (;line_length&&(line[line_length-1]==' '||line[line_length-1]=='\t'||line[line_length-1]=='\r');line_length--);
		if (!line_length||line[0]=='$'){
			continue;
		}
		line[line_length]=0;
		if (('A'<=line[0]&&line[0]<='Z')||('a'<=line[0]&&line[0]<='z')){
			line_index=0;
			if (CHECK_STR(line,"coordinates")){
				state=PARSER_STATE_COORDINATES;
			}
			else if (CHECK_STR(line,"connectivity")){
				state=PARSER_STATE_CONNECTIVITY;
			}
			else if (CHECK_STR(line,"geometry")){
				state=PARSER_STATE_GEOMETRY;
			}
			else if (CHECK_STR(line,"point load")){
				state=PARSER_STATE_POINT_LOAD;
			}
			else if (state==PARSER_STATE_GEOMETRY&&current_geometry_key){
				state=PARSER_STATE_GEOMETRY_CONTENT;
				_ensure_material_type_key(current_geometry_key);
				if (CHECK_STR(line,"corner_profile")){
					(_material_types+current_geometry_key)->bias=1.4559406779661016e-09;
				}
				else if (CHECK_STR(line,"single_strip")){
					(_material_types+current_geometry_key)->bias=3.375e-11;
				}
				else if (CHECK_STR(line,"double_strip")){
					(_material_types+current_geometry_key)->bias=2.7e-10;
				}
				else{
					ASSERT(!"Unknown material type");
				}
			}
			else{
				state=PARSER_STATE_NONE;
			}
			continue;
		}
		if (state==PARSER_STATE_COORDINATES&&line_index){
			uint32_t index=0;
			double x=0.0f;
			int32_t x_exp=0;
			double y=0.0f;
			int32_t y_exp=0;
			double z=0.0f;
			int32_t z_exp=0;
			ASSERT(sscanf(line,"%u%lf%d%lf%d%lf%d",&index,&x,&x_exp,&y,&y_exp,&z,&z_exp)==7);
			if (_point_count<index+1){
				_point_count=index+1;
				_points=realloc(_points,_point_count*sizeof(point_t));
			}
			(_points+index)->x=x*exp10(x_exp);
			(_points+index)->y=y*exp10(y_exp);
			(_points+index)->z=z*exp10(z_exp);
		}
		else if (state==PARSER_STATE_CONNECTIVITY){
			if (!line_index){
				ASSERT(sscanf(line,"%*d%*d%*d%*d%u",&current_geometry_key)==1);
				_ensure_material_type_key(current_geometry_key);
			}
			else{
				material_type_t* material_type=_material_types+current_geometry_key;
				material_type->connection_count++;
				material_type->connections=realloc(material_type->connections,material_type->connection_count*sizeof(connection_t));
				ASSERT(sscanf(line,"%*d%*d%u%u",&((material_type->connections+material_type->connection_count-1)->a),&((material_type->connections+material_type->connection_count-1)->b))==2);
			}
		}
		else if (state==PARSER_STATE_GEOMETRY&&line_index){
			ASSERT(sscanf(line,"%u",&current_geometry_key)==1);
		}
		else if (state==PARSER_STATE_GEOMETRY_CONTENT){
			double area=0.0;
			int32_t exp=0;
			ASSERT(sscanf(line,"%lf%d",&area,&exp)==2);
			(_material_types+current_geometry_key)->bias*=exp10(-exp); // multiplied before for better numerical stability
			(_material_types+current_geometry_key)->bias*=PI_SQUARED/area;
			state=PARSER_STATE_GEOMETRY;
			current_geometry_key=0;
		}
		else if (state==PARSER_STATE_POINT_LOAD&&line_index){
			_point_load_line_start=start_line_index;
			_point_load_line_end=ftell(file);
			state=PARSER_STATE_NONE;
		}
		line_index++;
	}
_eof:
	_job_file_length=ftell(file);
	fclose(file);
}



static void _prepare_output_directory(const char* path,const char* vm_path){
	_output_file_path_prefix_length=strlen(path)+1;
	memcpy(_output_file_path,path,_output_file_path_prefix_length-1);
	_output_file_path[_output_file_path_prefix_length-1]='/';
	_vm_output_dir=vm_path;
}



static void _simulate_step(const char* ssh_username,const char* ssh_password,const char* ssh_address){
	char script_buffer[4096];
	strcpy(_output_file_path+_output_file_path_prefix_length,"exec.bat");
	uint32_t script_buffer_length=snprintf(script_buffer,sizeof(script_buffer),_ssh_script,ssh_username);
	int fd=open(_output_file_path,O_WRONLY|O_CREAT|O_TRUNC,0666);
	ASSERT(write(fd,script_buffer,script_buffer_length)==script_buffer_length);
	close(fd);
	char line[256];
	for (uint32_t i=0;i<=_divisions;i++){
		snprintf(_output_file_path+_output_file_path_prefix_length,sizeof(_output_file_path)-_output_file_path_prefix_length,"_job%u.dat",i+1);
		fd=open(_output_file_path,O_WRONLY|O_CREAT|O_TRUNC,0666);
		off_t offset=0;
		ASSERT(sendfile(fd,_src_file_fd,&offset,_point_load_line_start)==_point_load_line_start);
		double value=_bounds[0]+(_bounds[1]-_bounds[0])/(_divisions+1)*i;
		int32_t exp=(value?log10(fabs(value)):0);
		uint32_t line_length=snprintf(line,sizeof(line)," 0.000000000000000+0%+.15f%+d 0.000000000000000+0\n",value*exp10(-exp),exp);
		ASSERT(write(fd,line,line_length)==line_length);
		offset=_point_load_line_end;
		ASSERT(sendfile(fd,_src_file_fd,&offset,_job_file_length-_point_load_line_end)==_job_file_length-_point_load_line_end);
		close(fd);
	}
	char command_buffer[256];
	snprintf(command_buffer,sizeof(command_buffer),_ssh_command,ssh_password,ssh_username,ssh_address,_vm_output_dir,_divisions+1);
	ASSERT(!system(command_buffer));
}



static void _update_bounds(void){
	_Bool prev_step_was_buckling=0;
	uint32_t i=0;
	for (;i<=_divisions;i++){
		char line[4096];
		snprintf(_output_file_path+_output_file_path_prefix_length,sizeof(_output_file_path)-_output_file_path_prefix_length,"_job%u.t19",i+1);
		FILE* file=fopen(_output_file_path,"rb");
		uint32_t state=PARSER_STATE_NONE;
		int32_t point_index=-3;
		while (1){
			char c;
			do{
				c=fgetc(file);
				if (c==EOF){
					goto _eof;
				}
			} while (c=='\n');
			uint32_t line_length=0;
			for (;c!=EOF&&c!='\n';line_length++){
				line[line_length]=c;
				c=fgetc(file);
			}
			for (;line_length&&(line[line_length-1]==' '||line[line_length-1]=='\t'||line[line_length-1]=='\r');line_length--);
			if (!line_length){
				continue;
			}
			line[line_length]=0;
			if (line[0]=='='||('A'<=line[0]&&line[0]<='Z')||('a'<=line[0]&&line[0]<='z')){
				if (CHECK_STR_PREFIX(line,"=beg=52401")){
					state=PARSER_STATE_NODE_RESULTS;
				}
				else if (state==PARSER_STATE_NODE_RESULTS&&CHECK_STR_PREFIX(line,"Displacement")){
					state=PARSER_STATE_DISPLACEMENTS;
					point_index=-3;
				}
				else{
					state=PARSER_STATE_NONE;
				}
				continue;
			}
			if (state!=PARSER_STATE_DISPLACEMENTS){
				continue;
			}
			for (uint32_t j=0;point_index<((int32_t)_point_count)&&j<2;j++){
				double x=0.0;
				double y=0.0;
				double z=0.0;
				ASSERT(sscanf(line+j*39,"%lf%lf%lf",&x,&y,&z)==3);
				if (point_index>=0){
					(_points+point_index)->dx=x;
					(_points+point_index)->dy=y;
					(_points+point_index)->dz=z;
				}
				point_index++;
			}
		}
_eof:
		fclose(file);
		if (point_index!=_point_count){
			printf("Incomplete marc result file: %s\n",_output_file_path);
			_Exit(1);
		}
		_Bool buckling=0;
		for (uint32_t j=0;j<_material_type_count;j++){
			const material_type_t* material_type=_material_types+j;
			for (uint32_t k=0;k<material_type->connection_count;k++){
				const point_t* a=_points+(material_type->connections+k)->a;
				const point_t* b=_points+(material_type->connections+k)->b;
				double l0=0.0;
				double l1=0.0;
				for (uint32_t l=0;l<3;l++){
					double delta=a->coords[l]-b->coords[l];
					l0+=delta*delta;
					delta+=a->coords[l+3]-b->coords[l+3];
					l1+=delta*delta;
				}
				if (l0-sqrt(l0*l1)>material_type->bias){
					buckling=1;
					goto _buckling_found;
				}
			}
		}
_buckling_found:
		if (!i){
			prev_step_was_buckling=buckling;
			continue;
		}
		if (prev_step_was_buckling!=buckling){
			break;
		}
	}
	float delta=(_bounds[1]-_bounds[0])/(_divisions+1);
	_bounds[0]+=delta*(i-1);
	_bounds[1]=_bounds[0]+delta;
}



int main(int argc,const char*const*argv){
	if (argc<10){
		printf("%s <marc_input_file> <host_file_directory> <vm_file_directory> <vm_ssh_username> <vm_ssh_password> <vm_ssh_address> <min_force> <max_force> <divisions>\n",(argc?argv[0]:"<executable>"));
		return 1;
	}
	_load_job_data(argv[1]);
	_prepare_output_directory(argv[2],argv[3]);
	ASSERT(sscanf(argv[7],"%lf",_bounds)==1&&sscanf(argv[8],"%lf",_bounds+1)==1&&sscanf(argv[9],"%u",&_divisions)==1&&_divisions);
	while (round(_bounds[0])!=round(_bounds[1])){
		_simulate_step(argv[4],argv[5],argv[6]);
		_update_bounds();
		printf("%.2lf, %.2lf\n",_bounds[0],_bounds[1]);
	}
	return 0;
}
