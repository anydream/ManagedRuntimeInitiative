/*
 * Copyright 2003-2005 Sun Microsystems, Inc.  All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 */

/*
 * @test
 * @bug 4635454 6208022
 * @summary Check pluggability of SSLSocketFactory and
 * SSLServerSocketFactory classes.
 */

import java.util.*;

import java.security.*;
import java.net.*;
import javax.net.ssl.*;

public class CheckSockFacExport1 {

    public static void main(String argv[]) throws Exception {
        Security.setProperty("ssl.SocketFactory.provider",
                             "MySSLSocketFacImpl");
        MySSLSocketFacImpl.useCustomCipherSuites();
        Security.setProperty("ssl.ServerSocketFactory.provider",
            "MySSLServerSocketFacImpl");
        MySSLServerSocketFacImpl.useCustomCipherSuites();

        String[] supportedCS = null;
        for (int i = 0; i < 2; i++) {
            switch (i) {
            case 0:
                System.out.println("Testing SSLSocketFactory:");
                SSLSocketFactory sf = (SSLSocketFactory)
                    SSLSocketFactory.getDefault();
                supportedCS = sf.getSupportedCipherSuites();
                break;
            case 1:
                System.out.println("Testing SSLServerSocketFactory:");
                SSLServerSocketFactory ssf = (SSLServerSocketFactory)
                    SSLServerSocketFactory.getDefault();
                supportedCS = ssf.getSupportedCipherSuites();
                break;
            default:
                throw new Exception("Internal Test Error");
            }
            System.out.println(Arrays.asList(supportedCS));
            if (supportedCS.length == 0) {
                throw new Exception("supported ciphersuites are empty");
            }
        }
        System.out.println("Test Passed");
    }
}
