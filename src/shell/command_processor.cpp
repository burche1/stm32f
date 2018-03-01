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

extern stm32f103::spi __spi0, __spi1;
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

size_t
strlen( const char * s )
{
    const char * p = s;
    while ( *p )
        ++p;
    return size_t( p - s );
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
    auto& spix = ( strcmp( argv[0], "spi") == 0 ) ? __spi0 : __spi1;

    if ( ! spix ) {
        using namespace stm32f103;
        stream() << argv[0] << " not initialized. -- initializing..." << std::endl;
        if ( strcmp( argv[0], "spi2" ) == 0 ) {

            if ( auto RCC = reinterpret_cast< volatile stm32f103::RCC * >( stm32f103::RCC_BASE ) )
                RCC->APB1ENR |= 1 << 14;    // SPI2

            // SPI
            // (see RM0008, p166, Table 25)
            //gpio_mode()( stm32f103::PB12, stm32f103::GPIO_CNF_ALT_OUTPUT_PUSH_PULL, stm32f103::GPIO_MODE_OUTPUT_50M ); // ~SS
            gpio_mode()( stm32f103::PB12, stm32f103::GPIO_CNF_OUTPUT_PUSH_PULL,     stm32f103::GPIO_MODE_OUTPUT_50M ); // ~SS
            
            gpio_mode()( stm32f103::PB13, stm32f103::GPIO_CNF_ALT_OUTPUT_PUSH_PULL, stm32f103::GPIO_MODE_OUTPUT_50M ); // SCLK
            gpio_mode()( stm32f103::PB14, stm32f103::GPIO_CNF_INPUT_FLOATING,       stm32f103::GPIO_MODE_INPUT );      // MISO
            gpio_mode()( stm32f103::PB15, stm32f103::GPIO_CNF_ALT_OUTPUT_PUSH_PULL, stm32f103::GPIO_MODE_OUTPUT_50M ); // MOSI
            spix.init( stm32f103::SPI2_BASE, 'B', 12 );
        } else {
            // spi1 should be initialized in main();
        }
    }
    
    // spi num
    size_t count = 1024;

    if ( argc >= 2 ) {
        count = strtod( argv[ 1 ] );
        if ( count == 0 )
            count = 1;
    }

    while ( count-- ) {
        uint32_t d = atomic_jiffies.load();
        spix << uint16_t( d & 0xffff );
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

    const size_t replicates = 0x7fffff;
    
    if ( argc >= 2 ) {
        const char * pin = argv[1];
        int no = 0;
        if (( pin[0] == 'P' && ( 'A' <= pin[1] && pin[1] <= 'C' ) ) && ( pin[2] >= '0' && pin[2] <= '9' ) ) {
            no = pin[2] - '0';
            if ( pin[3] >= '0' && pin[3] <= '9' )
                no = no * 10 + pin[3] - '0';

            stream() << "Pulse out to P" << pin[1] << no << std::endl;

            switch( pin[1] ) {
            case 'A':
                gpio_mode()( static_cast< GPIOA_PIN >(no), GPIO_CNF_OUTPUT_PUSH_PULL, GPIO_MODE_OUTPUT_50M );
                for ( size_t i = 0; i < replicates; ++i )
                    gpio< GPIOA_PIN >( static_cast< GPIOA_PIN >( no ) ) = bool( i & 01 );
                break;
            case 'B':
                gpio_mode()( static_cast< GPIOB_PIN >(no), GPIO_CNF_OUTPUT_PUSH_PULL, GPIO_MODE_OUTPUT_50M );
                for ( size_t i = 0; i < replicates; ++i )
                    gpio< GPIOB_PIN >( static_cast< GPIOB_PIN >( no ) ) = bool( i & 01 );
                break;                
            case 'C':
                gpio_mode()( static_cast< GPIOC_PIN >(no), GPIO_CNF_OUTPUT_PUSH_PULL, GPIO_MODE_OUTPUT_50M );
                for ( size_t i = 0; i < replicates; ++i )
                    gpio< GPIOC_PIN >( static_cast< GPIOC_PIN >( no ) ) = bool( i & 01 );
                break;                                
            }
            
        } else {
            stream() << "gpio 2nd argment format mismatch" << std::endl;
        }
    } else {
        stream() << "gpio <pin#>" << std::endl;
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
        using namespace stm32f103;
        
        stream() << "adc0 not initialized." << std::endl;
        
        // it must be initalized in main though, just in case
        gpio_mode()( stm32f103::PA0, GPIO_CNF_INPUT_ANALOG, GPIO_MODE_INPUT ); // ADC1 (0,0)
        
        __adc0.init( stm32f103::ADC1_BASE );
        stream() << "adc reset & calibration: status " << (( __adc0.cr2() & 0x0c ) == 0 ? " PASS" : " FAIL" )  << std::endl;
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
rcc_status( size_t argc, const char ** argv )
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

void
rcc_enable( size_t argc, const char ** argv )
{
    const char * myname = argv[ 0 ];
    
    if ( argc == 1 ) {
        int n = 0;
        for ( auto& p: __apb2enr__ ) {
            if ( p ) {
                if ( ( ++n % 8 ) == 0 )
                    stream() << std::endl;
                stream() << p << " | ";
            }
        }
        stream() << std::endl;
        n = 0;
        for ( auto& p: __apb1enr__ ) {
            if ( p ) {
                if ( ( ++n % 8 ) == 0 )
                    stream() << std::endl;                
                stream() << p << " | ";
            }
        }
        return;
    }
    --argc;
    ++argv;
    uint32_t flags1( 0 ), flags2( 0 );
    while ( argc-- ) {
        stream() << "looking for : " << argv[ 0 ] << std::endl;
        uint32_t i = 0;
        for ( auto& p: __apb2enr__ ) {
            if ( strcmp( p, argv[ 0 ] ) == 0 ) {
                flags2 |= (1 << i);
                stream() << "\tfound on APB2ENR: " << flags2 << std::endl;
            }
            ++i;
        }
        i = 0;
        for ( auto& p: __apb1enr__ ) {
            if ( strcmp( p, argv[ 0 ] ) == 0 ) {
                flags1 |= (1 << i);
                stream() << "\tfound on APB1ENR: " << flags1 << std::endl;
            }
            ++i;
        }
        ++argv;
    }
    if ( auto RCC = reinterpret_cast< volatile stm32f103::RCC * >( stm32f103::RCC_BASE ) ) {
        auto prev1 = RCC->APB1ENR;
        auto prev2 = RCC->APB2ENR;
        stream() << myname << " : " << flags2 << ", " << flags1 << std::endl;
        if ( strcmp( myname, "enable" ) == 0 ) {
            if ( flags2 ) {
                RCC->APB2ENR |= flags2;
                stream() << "APB2NER: " << prev2 << " | " << flags2 << "->" << RCC->APB2ENR << std::endl;
            }
            if ( flags1 ) {
                RCC->APB1ENR |= flags1;
                stream() << "APB1NER: " << prev1 << " | " << flags2 << "->" << RCC->APB1ENR << std::endl;
            }
        } else {
            if ( flags2 ) {
                RCC->APB2ENR &= ~flags2;
                stream() << "APB2NER: " << prev2 << " & " << ~flags2 << "->" << RCC->APB2ENR << std::endl;
            }
            if ( flags1 ) {
                RCC->APB1ENR &= ~flags1;
                stream() << "APB1NER: " << prev1 << " & " << ~flags1 << "->" << RCC->APB1ENR << std::endl;
            }
        }
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
    { "spi",    spi_test,       " spi [replicates]" }
    , { "spi2", spi_test,       " spi2 [replicates]" }
    , { "alt",  alt_test,       " spi [remap]" }
    , { "gpio", gpio_test,      " pin# (toggle PA# as GPIO, where # is 0..12)" }
    , { "adc",  adc_test,       " replicates (1)" }
    , { "ctor", ctor_test,      "" }
    , { "rcc",  rcc_status,       " RCC clock enable register list" }
    , { "disable", rcc_enable,  " reg1 [reg2...] Disable clock for specified peripheral." }
    , { "enable", rcc_enable,   " reg1 [reg2...] Enable clock for specified peripheral." }
    , { "afio", afio_test,      " AFIO MAPR list" }
};

bool
command_processor::operator()( size_t argc, const char ** argv ) const
{
    if ( argc ) {
        stream() << "command_procedssor: argc=" << argc << " argv = {";
        for ( size_t i = 0; i < argc; ++i )
            stream() << argv[i] << ( ( i < argc - 1 ) ? ", " : "" );
        stream() << "}" << std::endl;

        bool processed( false );
    
        if ( argc > 0 ) {
            for ( auto& cmd: command_table ) {
                if ( strcmp( cmd.arg0_, argv[0] ) == 0 ) {
                    processed = true;
                    cmd.f_( argc, argv );
                    break;
                }
            }
        }
        
        if ( ! processed ) {
            stream() << "command processor -- help" << std::endl;
            for ( auto& cmd: command_table )
                stream() << "\t" << cmd.arg0_ << cmd.help_ << std::endl;

            stream() << "----------------- RCC -----------------" << std::endl;
            static const char * rcc_argv[] = { "rcc", 0 };
            rcc_status( 1, rcc_argv );
        }
    }
    stream() << std::endl;
}
