// Copyright (C) 2018 MS-Cheminformatics LLC
// Licence: CC BY-NC
// Author: Toshinobu Hondo, Ph.D.
// Contact: toshi.hondo@qtplatz.com
//

#include "command_processor.hpp"
#include "adc.hpp"
#include "gpio.hpp"
#include "gpio_mode.hpp"
#include "printf.h"
#include "spi.hpp"
#include "stream.hpp"
#include "stm32f103.hpp"
#include <atomic>
#include <algorithm>
#include <functional>

extern stm32f103::spi __spi0;
extern stm32f103::adc __adc0;

extern std::atomic< uint32_t > atomic_jiffies;
extern void mdelay( uint32_t ms );

int
strcmp( const char * a, const char * b )
{
    while ( *a && (*a == *b ) )
        a++, b++;
    return *reinterpret_cast< const unsigned char *>(a) - *reinterpret_cast< const unsigned char *>(b);
}

int
strtod( const char * s )
{
    int sign = 1;
    int num = 0;
    while ( *s ) {
        if ( *s == '-' )
            sign = -1;
        else if ( '0' <= *s && *s <= '9' )
            num += (num * 10) + ((*s) - '0');
        ++s;
    }
    return num * sign;
}

void
spi_test( size_t argc, const char ** argv )
{
    // spi num
    size_t count = 1024;

    if ( argc >= 2 ) {
        count = strtod( argv[ 1 ] );
        if ( count == 0 )
            count = 1;
    }

    while ( count-- ) {
        uint32_t d = atomic_jiffies.load();
        __spi0 << ( d & 0xffff );
        mdelay( 100 );
    }
}

void
alt_test( size_t argc, const char ** argv )
{
    // alt spi [remap]
    using namespace stm32f103;    
    auto AFIO = reinterpret_cast< volatile stm32f103::AFIO * >( stm32f103::AFIO_BASE );
    if ( argc > 1 && strcmp( argv[1], "spi" ) == 0 ) {
        if ( argc == 2 ) {
            gpio_mode()( stm32f103::PA4, GPIO_CNF_ALT_OUTPUT_PUSH_PULL, GPIO_MODE_OUTPUT_50M ); // ~SS
            gpio_mode()( stm32f103::PA5, GPIO_CNF_ALT_OUTPUT_PUSH_PULL, GPIO_MODE_OUTPUT_50M ); // SCLK
            gpio_mode()( stm32f103::PA6, GPIO_CNF_INPUT_FLOATING,       GPIO_MODE_INPUT );      // MISO
            gpio_mode()( stm32f103::PA7, GPIO_CNF_ALT_OUTPUT_PUSH_PULL, GPIO_MODE_OUTPUT_50M ); // MOSI
            AFIO->MAPR &= ~1;            // clear SPI1 remap
        }
        if ( argc > 2 && strcmp( argv[2], "remap" ) == 0 ) {
            gpio_mode()( stm32f103::PA15, GPIO_CNF_ALT_OUTPUT_PUSH_PULL, GPIO_MODE_OUTPUT_50M ); // NSS
            gpio_mode()( stm32f103::PB3,  GPIO_CNF_ALT_OUTPUT_PUSH_PULL, GPIO_MODE_OUTPUT_50M ); // SCLK
            gpio_mode()( stm32f103::PB4,  GPIO_CNF_INPUT_FLOATING,       GPIO_MODE_INPUT );      // MISO
            gpio_mode()( stm32f103::PB5,  GPIO_CNF_ALT_OUTPUT_PUSH_PULL, GPIO_MODE_OUTPUT_50M ); // MOSI
            AFIO->MAPR |= 1;          // set SPI1 remap
        }
        stream() << "\tEnable SPI, AFIO MAPR: 0x" << AFIO->MAPR << std::endl;
    } else {
        stream()
            << "\tError: insufficent arguments\nalt spi [remap]"
            << "\tAFIO MAPR: 0x" << AFIO->MAPR
            << std::endl;
    }
}

void
gpio_test( size_t argc, const char ** argv )
{
    using namespace stm32f103;
    
    if ( argc >= 2 ) {
        if ( strcmp( argv[1], "spi" ) == 0 ) {
            gpio_mode()( PA4, GPIO_CNF_OUTPUT_PUSH_PULL, GPIO_MODE_OUTPUT_2M );
            gpio_mode()( PA5, GPIO_CNF_OUTPUT_PUSH_PULL, GPIO_MODE_OUTPUT_2M );
            gpio_mode()( PA6, GPIO_CNF_OUTPUT_PUSH_PULL, GPIO_MODE_OUTPUT_2M );
            gpio_mode()( PA7, GPIO_CNF_OUTPUT_PUSH_PULL, GPIO_MODE_OUTPUT_2M );
            for ( int i = 0; i < 0xfffff; ++i ) {
                stm32f103::gpio< decltype( stm32f103::PA4 ) >( stm32f103::PA4 ) = bool( i & 01 );
                stm32f103::gpio< decltype( stm32f103::PA5 ) >( stm32f103::PA5 ) = bool( i & 01 );
                stm32f103::gpio< decltype( stm32f103::PA6 ) >( stm32f103::PA6 ) = bool( i & 01 );
                stm32f103::gpio< decltype( stm32f103::PA7 ) >( stm32f103::PA7 ) = bool( i & 01 );
                auto tp = atomic_jiffies.load();
                while ( tp == atomic_jiffies.load() ) // 100us
                    ;
            }
        }
    } else {
        stream() << "gpio spi" << std::endl;
    }
}


void
adc_test( size_t argc, const char ** argv )
{
    size_t count = 1;

    if ( argc >= 2 ) {
        count = strtod( argv[ 1 ] );
        if ( count == 0 )
            count = 1;
    }

    if ( !__adc0 ) {
        stream() << "adc0 not initialized." << std::endl;
        __adc0.init( stm32f103::ADC1_BASE );
        stream() << "adc reset & calibration: status = " << ( __adc0.cr2() & 0x0c ) << std::endl;
    }

    for ( size_t i = 0; i < count; ++i ) {
        if ( __adc0.start_conversion() ) { // software trigger
            uint32_t d = __adc0.data(); // can't read twince
            stream() << "[" << int(i) << "] adc data= 0x" << d
                     << "\t" << int(d) << "(mV)"
                     << std::endl;
        }
    }
}

class ctor {
public:
    ctor() {
        stream() << "ctor constracted" << std::endl;
    };
    ~ctor() {
        stream() << "~ctor destracted" << std::endl;
    };
};

void
ctor_test( size_t argc, const char ** argv )
{
    ctor x;
}

static const char * __apb2enr__ [] = {
    "AFIO",    nullptr, "IOPA",  "IOPB",  "IOPC",  "IOPD",  "IOPE",  "IOPF"
    , "IOPG",  "ADC1",  "ADC2",  "TIM1",  "SPI1",  "TIM8",  "USART1","ADC3"
    , nullptr, nullptr, nullptr, "TIM9",  "TIM10", "TIM11"
};

static const char * __apb1enr__ [] = {
    "TIM2",     "TIM3",  "TIM4", "TIM5",  "TIM6",  "TIM7",  "TIM12", "TIM13"
    , "TIM14", nullptr, nullptr, "WWDG",  nullptr, nullptr, "SPI2",  "SPI3"
    , nullptr,"USART2","USART3", "USART4","USART5","I2C1",  "I2C2",  "USB"
    , nullptr,   "CAN", nullptr, "BPK",   "PWR",    "DAC"
};

void
rcc_test( size_t argc, const char ** argv )
{
    if ( auto RCC = reinterpret_cast< volatile stm32f103::RCC * >( stm32f103::RCC_BASE ) ) {
        stream() << "APB2, APB1 peripheral clock enable register (p112-116, RM0008, Rev 17) " << std::endl;
        stream() << "\tRCC->APB2ENR : " << RCC->APB2ENR << std::endl;
        stream() << "\tRCC->APB1ENR : " << RCC->APB1ENR << std::endl;
        
        {
            stream() << "\tEnables : ";
            int i = 0;
            for ( auto& p: __apb2enr__ ) {
                if ( p && ( RCC->APB2ENR & (1<<i) ) )
                    stream() << p << ", ";                    
                ++i;
            }
            stream() << "|";
        }
        {
            int i = 0;
            for ( auto& p: __apb1enr__ ) {
                if ( p && ( RCC->APB1ENR & (1<<i) ) )
                    stream() << p << ", ";
                ++i;
            }
        }
        stream() << std::endl;
    }
}

void
afio_test( size_t argc, const char ** argv )
{
    if ( auto AFIO = reinterpret_cast< volatile stm32f103::AFIO * >( stm32f103::AFIO_BASE ) ) {    
        stream() << "\tAFIO MAPR: 0x" << AFIO->MAPR << std::endl;
    }
}


///////////////////////////////////////////////////////

command_processor::command_processor()
{
}

class premitive {
public:
    const char * arg0_;
    void (*f_)(size_t, const char **);
    const char * help_;
};

static const premitive command_table [] = {
    { "spi",    spi_test,   " [number]" }
    , { "alt",  alt_test,   " spi [remap]" }
    , { "gpio", gpio_test,  " spi -- (toggles A4-A7 as GPIO)" }
    , { "adc",  adc_test,   "" }
    , { "ctor", ctor_test,  "" }
    , { "rcc",  rcc_test,   "" }
    , { "afio", afio_test,   "" }        
};

bool
command_processor::operator()( size_t argc, const char ** argv ) const
{
    stream() << "command_procedssor: argc=" << argc << " argv = {";
    for ( size_t i = 0; i < argc; ++i )
         stream() << argv[i] << ( ( i < argc - 1 ) ? ", " : "" );
    stream() << "}" << std::endl;
    
    if ( argc > 0 ) {
        for ( auto& cmd: command_table ) {
            if ( strcmp( cmd.arg0_, argv[0] ) == 0 ) {
                cmd.f_( argc, argv );
                break;
            }
        }
    } else {
        stream() << "command processor -- help" << std::endl;
        for ( auto& cmd: command_table )
            stream() << "\t" << cmd.arg0_ << cmd.help_ << std::endl;
    }
}
